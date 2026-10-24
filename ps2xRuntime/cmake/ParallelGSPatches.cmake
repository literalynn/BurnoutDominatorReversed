# Fixes applied to the pinned paraLLEl-GS sources at configure time. Each
# patched file is written to the build tree and replaces the original source.
function(parallel_gs_replace before after)
    string(REPLACE "\r\n" "\n" patch_source "${patch_source}") # checkouts with core.autocrlf
    string(FIND "${patch_source}" "${before}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "paraLLEl-GS patch for ${patch_file} does not match the pinned source")
    endif()
    string(REPLACE "${before}" "${after}" patch_source "${patch_source}")
    set(patch_source "${patch_source}" PARENT_SCOPE)
endfunction()

function(parallel_gs_use_patched relative output)
    set(patched "${parallel_gs_BINARY_DIR}/${output}")
    file(CONFIGURE OUTPUT "${patched}" CONTENT "${patch_source}" @ONLY)
    get_target_property(sources parallel-gs SOURCES)
    get_filename_component(name "${relative}" NAME)
    list(REMOVE_ITEM sources "${name}")
    set_property(TARGET parallel-gs PROPERTY SOURCES "${sources}")
    target_sources(parallel-gs PRIVATE "${patched}")
endfunction()

# From upstream PS2Recomp pull request #285: transfers whose size is not a
# multiple of 64 bits (upload) or 128 bits (readback) keep their last bytes.
set(patch_file gs/gs_interface.cpp)
file(READ "${parallel_gs_SOURCE_DIR}/${patch_file}" patch_source)
parallel_gs_replace("get_bits_per_pixel(transfer_state.copy.bitbltbuf.desc.DPSM)) / 64;" "get_bits_per_pixel(transfer_state.copy.bitbltbuf.desc.DPSM) + 63) / 64;")
parallel_gs_replace("get_bits_per_pixel(transfer_state.copy.bitbltbuf.desc.SPSM)) / 8;" "get_bits_per_pixel(transfer_state.copy.bitbltbuf.desc.SPSM) + 7) / 8;")
parallel_gs_replace("transfer_state.fifo_readback.reserve(required_bytes);" "transfer_state.fifo_readback.reserve((required_bytes + 15) & ~15u);\n\t\tif (required_bytes) memset(transfer_state.fifo_readback.data(), 0, (required_bytes + 15) & ~15u);")
parallel_gs_replace("transfer_state.fifo_readback_128b_size = required_bytes / 16;" "transfer_state.fifo_readback_128b_size = (required_bytes + 15) / 16;")
# HUD smoothing: sprites snap their texture coordinates to pixel centres, so every sample of
# a pixel reads the same texel and a bilinear HUD texture shows its texels as blocks at 2x
# and 4x. Sprites that sample a palette texture (PSMT8, PSMT4: HUD and menu art, never a
# render target) with LINEAR magnification rasterize and interpolate per sample when
# bdr_smooth_ui_sprites (the ubershader snaps attributes along with the raster).
parallel_gs_replace("void GSInterface::set_hacks(const Hacks &hacks_)" "bool bdr_smooth_ui_sprites = false;\n\nvoid GSInterface::set_hacks(const Hacks &hacks_)")
parallel_gs_replace("\t\tprim_attr.state |= 1u << STATE_BIT_SNAP_RASTER;\n\t\tprim_attr.state |= 1u << STATE_BIT_SNAP_ATTRIBUTE;\n\t\tprim_attr.state &= ~(1u << STATE_BIT_MULTISAMPLE);"
    "\t\tif (!(bdr_smooth_ui_sprites && prim.desc.TME && ctx.tex1.desc.MMAG == TEX1Bits::LINEAR &&\n\t\t      (ctx.tex0.desc.PSM == PSMT8 || ctx.tex0.desc.PSM == PSMT4)))\n\t\t\tprim_attr.state |= (1u << STATE_BIT_SNAP_RASTER) | (1u << STATE_BIT_SNAP_ATTRIBUTE);\n\t\tprim_attr.state &= ~(1u << STATE_BIT_MULTISAMPLE);")
parallel_gs_use_patched(${patch_file} gs_interface_patched.cpp)

# compute_page_rect: max_block_x/y are taken modulo the blocks of a page, so
# for a texture wider or taller than a page row with a base that is not page
# aligned they can be smaller than min_block_x/y. The block count then wraps
# to about 2^32 and swizzle_block_mask_PS2 never returns (Burnout Dominator's
# intro). Such a rect spans whole pages: treat it as straddling.
set(patch_file gs/gs_util.cpp)
file(READ "${parallel_gs_SOURCE_DIR}/${patch_file}" patch_source)
parallel_gs_replace("\tif (block_offset != 0 || single_page_rect)\n\t{"
    "\tif (block_offset != 0 && !single_page_rect && (max_block_x < (x >> layout.block_width_log2) || max_block_y < (y >> layout.block_height_log2)))\n\t{\n\t\trect.page_width++;\n\t}\n\telse if (block_offset != 0 || single_page_rect)\n\t{")
parallel_gs_use_patched(${patch_file} gs_util_patched.cpp)

# 4x scan-out: with 16 samples a pixel (ordered 4x4 grid) the high-resolution scan-out
# shows each sample as a pixel, 2560 pixels wide, instead of the four quadrant averages at
# 1280. The sample shader comes from ps2xRuntime/src/lib/gs/shaders/pgs_sample_circuit.frag
# (tools/pgs_shader.py); the rest scales the circuit images, scissors and viewports by 4.
set(patch_file gs/gs_renderer.cpp)
file(READ "${parallel_gs_SOURCE_DIR}/${patch_file}" patch_source)
parallel_gs_replace("#include \"shaders/slangmosh.hpp\"" "#include \"shaders/slangmosh.hpp\"\n#include \"pgs_sample_circuit.spv.h\"")
parallel_gs_replace("\tsample_quad[0] = device_->request_program(shaders.quad, shaders.sample_circuit[0]);"
    "\tsample_quad[0] = device_->request_program(shaders.quad, device_->request_shader(pgs_sample_circuit_spirv, sizeof(pgs_sample_circuit_spirv), &shaders.sample_circuit[0]->get_layout()));")
parallel_gs_replace("\tif (super_samples > 1)\n\t{\n\t\tvalid_extent.width *= 2;\n\t\tvalid_extent.height *= 2;\n\t}"
    "\tif (super_samples > 1)\n\t{\n\t\tvalid_extent.width <<= super_samples == 16 ? 2 : 1;\n\t\tvalid_extent.height <<= super_samples == 16 ? 2 : 1;\n\t}")
parallel_gs_replace("\tif (high_resolution_scanout)\n\t\tsuper_samples = 1 << (sampling_rate_x_log2 + sampling_rate_y_log2);\n"
    "\tif (high_resolution_scanout)\n\t\tsuper_samples = 1 << (sampling_rate_x_log2 + sampling_rate_y_log2);\n\tconst uint32_t scanout_scale_log2 = super_samples == 16 ? 2 : 1;\n")
parallel_gs_replace("\t\t\t\timage_info.width *= 2;\n\t\t\t\timage_info.height *= 2;"
    "\t\t\t\timage_info.width <<= scanout_scale_log2;\n\t\t\t\timage_info.height <<= scanout_scale_log2;")
parallel_gs_replace("\t\t\thoriz_resolution0 >>= 1;\n\t\t\thoriz_resolution1 >>= 1;"
    "\t\t\thoriz_resolution0 >>= scanout_scale_log2;\n\t\t\thoriz_resolution1 >>= scanout_scale_log2;")
parallel_gs_replace("\t\t\teffective_mode_width *= 2;\n\t\t\teffective_mode_height *= 2;"
    "\t\t\teffective_mode_width <<= scanout_scale_log2;\n\t\t\teffective_mode_height <<= scanout_scale_log2;")
parallel_gs_replace("int(high_resolution_scanout)" "(high_resolution_scanout ? scanout_scale_log2 : 0u)")
parallel_gs_replace("\t\t\t\tvp.x *= 2.0f;\n\t\t\t\tvp.y *= 2.0f;\n\t\t\t\tvp.width *= 2.0f;\n\t\t\t\tvp.height *= 2.0f;"
    "\t\t\t\tvp.x *= float(1u << scanout_scale_log2);\n\t\t\t\tvp.y *= float(1u << scanout_scale_log2);\n\t\t\t\tvp.width *= float(1u << scanout_scale_log2);\n\t\t\t\tvp.height *= float(1u << scanout_scale_log2);")
# The scan-out images (the circuits, the merged image, the deinterlaced one) are created at every
# vsync. NVIDIA's driver prefers colour attachments in allocations of their own, so each one was a
# vkAllocateMemory and a vkFreeMemory, 18 MiB at 4x resolution: at 240 frames a second about an
# eighth of the GPU submission thread. Granite now places them in its heaps, which it keeps.
parallel_gs_replace("\timage_info.misc |= Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;"
    "\timage_info.misc |= Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT | Vulkan::IMAGE_MISC_FORCE_NO_DEDICATED_BIT;")
parallel_gs_use_patched(${patch_file} gs_renderer_patched.cpp)
target_include_directories(parallel-gs PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src/lib/gs/shaders")
