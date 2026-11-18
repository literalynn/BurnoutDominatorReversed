"""Compiles the paraLLEl-GS shaders the runtime replaces into SPIR-V headers in ps2xRuntime/src/lib/gs/shaders:
pgs_sample_circuit.spv.h from pgs_sample_circuit.frag, pgs_triangle_setup.spv.h from the checkout's
gs/shaders/triangle_setup.comp with the changes below, pgs_ubershader.spv.h from its ubershader.comp with primitive
attributes readonly (glslangValidator without spirv-opt: AMD's and NVIDIA's drivers run it faster than upstream's
optimized build), pgs_sampler_feedback.spv.h from its sampler_feedback.comp and pgs_binning.spv.h from its
binning.comp, with the changes below.

usage: python tools/pgs_shader.py --glslang <glslangValidator> --pgs <paraLLEl-GS checkout>

glslangValidator builds from the checkout's Granite/third_party/glslang (target
glslang-standalone). The headers are committed, so building the game does not need it.
"""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADERS = ROOT / "ps2xRuntime" / "src" / "lib" / "gs" / "shaders"

# Flat triangles (no perspective, one depth) are taken for UI and interpolate their texture coordinates
# once per pixel, so a texture drawn 1:1 at the console's resolution shows its texels as 2x2 or 4x4
# blocks. parallel-gs/gs_interface.cpp sets bit 27 of PrimitiveAttribute::tex for the
# UI it smooths (palette textures with LINEAR magnification): those interpolate per sample like 3D.
TRIANGLE_SETUP_CHANGE = (
    "        bool should_interpolate_per_sample = multisample;\n",
    "        bool should_interpolate_per_sample = multisample || (primitive_attr.data[index].tex & (1u << 27)) != 0;\n")

# The ubershader never writes primitive attributes, but data_buffers.h declares them writable. Declared readonly
# (the header inlined with that change), the drivers load them through the scalar cache in the per-primitive
# loop instead of waiting on a vector load: 5% less shading time on AMD (an RTX 5090: within the noise).
ATTRIBUTES_READONLY = ("\nbuffer PrimitiveAttributes\n", "\nreadonly buffer PrimitiveAttributes\n")

# The texture analysis marks the 8x8 blocks each primitive's texture reads with an atomicOr per block, though
# neighbouring primitives mostly read the same blocks. A block whose bit already reads as set needs nothing: bits
# are only set during the dispatch (cleared before it, behind a barrier), and a 0 read too early still takes the
# atomic. Half the analysis time on a 2-CU Radeon iGPU (0.95 to 0.49 ms a race frame at 1 sample).
SAMPLER_FEEDBACK_CHANGE = (
    "    uint mask = 1u << (linear_block & 31u);\n    uint old_mask = atomicOr(",
    "    uint mask = 1u << (linear_block & 31u);\n    if ((bitmask_va.data[linear_block / 32] & mask) != 0u)\n"
    "        return;\n    uint old_mask = atomicOr(")

# Binning's coarse loop tests every primitive of a render pass against each workgroup's area, reading the
# primitive's state and its 80-byte setup for it. Triangle setup also writes each primitive's bounding box, clamped
# to [-1, 32767] and packed in 16 bits a coordinate, into a buffer of its own (binding 23, gs_renderer.cpp), and the
# coarse loop reads that 8-byte box first: a box that misses the area fails bin_primitive's own box test, so state
# and setup are only read for the others. The clamped box meets the area exactly when the full one does while the
# area lies within [0, 32766]; outside it the loop reads every state as before. Binning takes 40 to 50% less time on
# a 2-CU Radeon iGPU.
PRIMITIVE_BOXES = """#include "data_buffers.h"
layout(set = 0, binding = 23, std430) {q} buffer BdrPrimitiveBoxes
{{
    uvec2 data[];
}} bdr_boxes;
"""
TRIANGLE_SETUP_BOXES = [
    ('#include "data_buffers.h"\n', PRIMITIVE_BOXES.format(q="writeonly")),
    ("\nvoid main_inner(uint index)", """
uvec2 bdr_pack_box(ivec4 bb)
{
    bb = clamp(bb, ivec4(-1), ivec4(32767));
    return uvec2((uint(bb.x) & 0xffffu) | (uint(bb.y) << 16u), (uint(bb.z) & 0xffffu) | (uint(bb.w) << 16u));
}

void main_inner(uint index)"""),
    ("        primitive_setup.data[index].bb = ivec4(0, 0, -1, -1);\n",
     "        primitive_setup.data[index].bb = ivec4(0, 0, -1, -1);\n"
     "        bdr_boxes.data[index] = bdr_pack_box(ivec4(0, 0, -1, -1));\n"),
    ("    primitive_setup.data[index].bb = packed_bb;\n",
     "    primitive_setup.data[index].bb = packed_bb;\n    bdr_boxes.data[index] = bdr_pack_box(packed_bb);\n"),
]
BINNING_BOXES = [
    ('#include "data_buffers.h"\n', PRIMITIVE_BOXES.format(q="readonly")),
    ("""    if (HIER_CULLING_SIZE > 1)
    {
        init_shared();
""", """    if (HIER_CULLING_SIZE > 1)
    {
        init_shared();
        // The clamped boxes give the same overlap as the full ones while the area lies within [0, 32766].
        bool bdr_full_test = any(lessThan(full_tile_lo, ivec2(0))) || any(greaterThan(full_tile_hi, ivec2(32766)));
"""),
    ("""            uint s = primitive_attr.data[i].state;
            if (bitfieldExtract(s,""", """            uvec2 bdr_box = bdr_boxes.data[i];
            ivec4 bdr_bb = ivec4(bitfieldExtract(int(bdr_box.x), 0, 16), bitfieldExtract(int(bdr_box.x), 16, 16),
                                 bitfieldExtract(int(bdr_box.y), 0, 16), bitfieldExtract(int(bdr_box.y), 16, 16));
            bdr_bb = clip_bounding_box(bdr_bb, ivec4(full_tile_lo, full_tile_hi));
            // A box that misses the area fails bin_primitive's own box test: its state is not needed.
            uint s = ~0u;
            if (bdr_full_test || all(greaterThanEqual(bdr_bb.zw, bdr_bb.xy)))
                s = primitive_attr.data[i].state;
            if (bitfieldExtract(s,"""),
]


def compile_header(glslang, pgs, source, name, origin, defines=()):
    with tempfile.TemporaryDirectory() as scratch:
        spv = pathlib.Path(scratch) / "out.spv"
        subprocess.run([glslang, "-V", "--target-env", "vulkan1.1",
                        "-I" + str(pgs / "gs" / "shaders"),
                        "-I" + str(pgs / "Granite" / "assets" / "shaders" / "inc"),
                        *defines, str(source), "-o", str(spv)], check=True)
        data = spv.read_bytes()
    assert len(data) % 4 == 0 and data[:4] == bytes.fromhex("03022307"), "not SPIR-V"
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    lines = ["// Generated by tools/pgs_shader.py from %s. Do not edit." % origin,
             "// SPDX-License-Identifier: LGPL-3.0+ (derived from paraLLEl-GS)",
             "#pragma once",
             "#include <stdint.h>",
             "static const uint32_t %s_spirv[] = {" % name]
    for i in range(0, len(words), 8):
        lines.append("\t" + ", ".join("0x%08xu" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    (SHADERS / (name + ".spv.h")).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("%s: %d words" % (name, len(words)))


def compile_pinned(glslang, pgs, file, name, origin, changes=(), defines=()):
    """Compiles gs/shaders/<file> of the checkout, with (old, new) source changes."""
    source = (pgs / "gs" / "shaders" / file).read_text(encoding="utf-8").replace("\r\n", "\n")
    for old, new in changes:
        assert source.count(old) == 1, "%s does not match the pinned paraLLEl-GS" % file
        source = source.replace(old, new)
    source = source.replace("#version 450\n", "#version 450\n#extension GL_GOOGLE_include_directive : require\n", 1)
    with tempfile.TemporaryDirectory() as scratch:
        patched = pathlib.Path(scratch) / file
        patched.write_text(source, encoding="utf-8", newline="\n")
        compile_header(glslang, pgs, patched, name, origin, defines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--glslang", required=True)
    parser.add_argument("--pgs", required=True, type=pathlib.Path)
    args = parser.parse_args()
    compile_header(args.glslang, args.pgs, SHADERS / "pgs_sample_circuit.frag", "pgs_sample_circuit",
                   "pgs_sample_circuit.frag (PROMOTED=0)", ["-DPROMOTED=0"])
    compile_pinned(args.glslang, args.pgs, "triangle_setup.comp", "pgs_triangle_setup",
                   "paraLLEl-GS gs/shaders/triangle_setup.comp, smoothed UI interpolated per sample, packed boxes",
                   [TRIANGLE_SETUP_CHANGE] + TRIANGLE_SETUP_BOXES)
    header = (args.pgs / "gs" / "shaders" / "data_buffers.h").read_text(encoding="utf-8").replace("\r\n", "\n")
    assert header.count(ATTRIBUTES_READONLY[0]) == 1, "data_buffers.h does not match the pinned paraLLEl-GS"
    compile_pinned(args.glslang, args.pgs, "ubershader.comp", "pgs_ubershader",
                   "paraLLEl-GS gs/shaders/ubershader.comp (FEEDBACK_COLOR=0, FEEDBACK_DEPTH=0), primitive attributes readonly",
                   [('#include "data_buffers.h"\n', header.replace(*ATTRIBUTES_READONLY) + "\n")],
                   ["-DFEEDBACK_COLOR=0", "-DFEEDBACK_DEPTH=0"])
    compile_pinned(args.glslang, args.pgs, "sampler_feedback.comp", "pgs_sampler_feedback",
                   "paraLLEl-GS gs/shaders/sampler_feedback.comp, blocks already marked skip the atomic", [SAMPLER_FEEDBACK_CHANGE])
    compile_pinned(args.glslang, args.pgs, "binning.comp", "pgs_binning",
                   "paraLLEl-GS gs/shaders/binning.comp, coarse loop reads the packed boxes first", BINNING_BOXES)


if __name__ == "__main__":
    main()
