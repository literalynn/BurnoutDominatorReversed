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
    "\t\textern bool bdr_smooth_ui_sprites; // defined further down; GCC and Clang need it declared before this template body\n\t\tif (!(bdr_smooth_ui_sprites && prim.desc.TME && ctx.tex1.desc.MMAG == TEX1Bits::LINEAR &&\n\t\t      (ctx.tex0.desc.PSM == PSMT8 || ctx.tex0.desc.PSM == PSMT4)))\n\t\t\tprim_attr.state |= (1u << STATE_BIT_SNAP_RASTER) | (1u << STATE_BIT_SNAP_ATTRIBUTE);\n\t\tprim_attr.state &= ~(1u << STATE_BIT_MULTISAMPLE);")
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
# Likewise the texture cache's images, which it keeps for reuse only at power-of-two sizes up to
# 1024: the others came and went with allocations of their own, another 5% of that thread.
parallel_gs_replace("\t\tinfo.misc |= Vulkan::IMAGE_MISC_CREATE_PER_MIP_LEVEL_VIEWS_BIT;"
    "\t\tinfo.misc |= Vulkan::IMAGE_MISC_CREATE_PER_MIP_LEVEL_VIEWS_BIT | Vulkan::IMAGE_MISC_FORCE_NO_DEDICATED_BIT;")
# Pipelines precompiled at start-up, on half the CPU's threads: on a first run or after a driver
# update the driver compiles them for a minute and more. The list now has the keys the renderer
# requests: a pass shades its super samples and then its single samples with the subgroup size
# range of its rate (upstream had every rate's constants with [16, 64], [8, 64] and [4, 64]
# invocations: not the ranges of passes at one and two samples a pixel, and on GPUs without wave64
# three keys for the same pipelines), the reference bit at single sampling whenever VRAM has room
# for samples, also with a texture array, and the uploads' fourth constant; only the rates
# set_super_sampling_rate can set. The copy, upload and binning shaders come first, then the
# configured rate and single sampling, and each thread takes every n-th task (they had slices, the
# remainder left out). A third less to compile on NVIDIA.
parallel_gs_replace("#include <climits>\n"
    "#include <climits>\n#include <unordered_set>\n")
parallel_gs_replace("void GSRenderer::kick_compilation_tasks()\n{\n"
    "static GSOptions bdr_precompile_options; // set by init\n\nvoid GSRenderer::kick_compilation_tasks()\n{\n")
parallel_gs_replace("\tstd::vector<Vulkan::DeferredPipelineCompile> tasks;\n\tcompilation_tasks_active = true;\n"
    "\tstd::vector<Vulkan::DeferredPipelineCompile> tasks;\n\tsize_t bdr_ubershader_end = 0;\n\tcompilation_tasks_active = true;\n")
parallel_gs_replace("\t\tstatic const struct\n\t\t{\n\t\t\tuint32_t lo, hi;\n\t\t} subgroup_configs[] = {\n\t\t\t{ 6, 6 },\n\t\t\t{ 4, 6 },\n\t\t\t{ 3, 6 },\n\t\t\t{ 2, 6 },\n\t\t};\n\n\t\tfor (auto &subgroup_config : subgroup_configs)\n\t\t{\n\t\t\tif (device->supports_subgroup_size_log2(true, subgroup_config.lo, subgroup_config.hi))\n\t\t\t\tcmd->set_subgroup_size_log2(true, subgroup_config.lo, subgroup_config.hi);\n\t\t\telse\n\t\t\t\tcontinue;\n\n\t\t\tfor (auto &format : formats)\n\t\t\t{\n\t\t\t\tfor (auto &flags : variant_flags)\n\t\t\t\t{\n\t\t\t\t\tfor (auto &rates : sampling_rates)\n\t\t\t\t\t{\n"
    "\t\t// The rates set_super_sampling_rate picks (sampling_rates index) up to the supported maximum.\n\t\tconst auto rate_index = [&](uint32_t samples) -> uint32_t {\n\t\t\tconst bool ordered = bdr_precompile_options.ordered_super_sampling;\n\t\t\tswitch (std::min<uint32_t>(samples, uint32_t(get_max_supported_super_sampling())))\n\t\t\t{\n\t\t\tcase 2: return 1;\n\t\t\tcase 4: return ordered ? 2 : 3;\n\t\t\tcase 8: return 4;\n\t\t\tcase 16: return ordered ? 5 : 6;\n\t\t\tdefault: return 0;\n\t\t\t}\n\t\t};\n\t\tuint32_t rate_order[7];\n\t\tuint32_t rate_count = 0;\n\t\tconst auto add_rate = [&](uint32_t index) {\n\t\t\tif (std::find(rate_order, rate_order + rate_count, index) == rate_order + rate_count)\n\t\t\t\trate_order[rate_count++] = index;\n\t\t};\n\t\tadd_rate(rate_index(uint32_t(bdr_precompile_options.super_sampling)));\n\t\tadd_rate(0);\n\t\tif (bdr_precompile_options.dynamic_super_sampling)\n\t\t\tfor (uint32_t samples = 2; samples <= 16; samples *= 2)\n\t\t\t\tadd_rate(rate_index(samples));\n\n\t\tfor (uint32_t order = 0; order < rate_count; order++)\n\t\t{\n\t\t\t// A pass at this rate shades with the subgroup size range dispatch_shading requests for\n\t\t\t// it: the super samples with the rate's constants, then the single samples with zeros.\n\t\t\tconst auto &pass_rates = sampling_rates[rate_order[order]];\n\t\t\tconst uint32_t rate_log2 = pass_rates.sample_x + pass_rates.sample_y;\n\t\t\tif (device->supports_subgroup_size_log2(true, 6, 6))\n\t\t\t\tcmd->set_subgroup_size_log2(true, 6, 6);\n\t\t\telse if (device->supports_subgroup_size_log2(true, rate_log2, 6))\n\t\t\t\tcmd->set_subgroup_size_log2(true, rate_log2, 6);\n\t\t\telse\n\t\t\t\tcontinue;\n\n\t\t\tfor (auto &format : formats)\n\t\t\t{\n\t\t\t\tfor (auto &flags : variant_flags)\n\t\t\t\t{\n\t\t\t\t\tfor (uint32_t single = 0; single < (rate_log2 ? 2u : 1u); single++)\n\t\t\t\t\t{\n\t\t\t\t\t\tconst auto &rates = single ? sampling_rates[0] : pass_rates;\n")
parallel_gs_replace("\t\t\t\t\t\t\tcmd->set_specialization_constant(5, active_flags);\n\t\t\t\t\t\t\tcmd->extract_pipeline_state(deferred);\n\t\t\t\t\t\t\ttasks.push_back(deferred);\n\n\t\t\t\t\t\t\tif (rates.sample_y != 0)\n"
    "\t\t\t\t\t\t\tcmd->set_specialization_constant(5, active_flags);\n\t\t\t\t\t\t\tcmd->extract_pipeline_state(deferred);\n\t\t\t\t\t\t\t// dispatch_shading sets the reference bit whenever VRAM has room for samples.\n\t\t\t\t\t\t\tif (rates.sample_y != 0 || !can_potentially_super_sample())\n\t\t\t\t\t\t\t\ttasks.push_back(deferred);\n\n\t\t\t\t\t\t\tif (rates.sample_y != 0)\n")
parallel_gs_replace("\t\t\t\t\t\t\t\tcmd->set_specialization_constant(\n\t\t\t\t\t\t\t\t\t5, flags | VARIANT_FLAG_HAS_SUPER_SAMPLE_REFERENCE_BIT);\n\t\t\t\t\t\t\t\tcmd->extract_pipeline_state(deferred);\n\t\t\t\t\t\t\t\ttasks.push_back(deferred);\n"
    "\t\t\t\t\t\t\t\tcmd->set_specialization_constant(\n\t\t\t\t\t\t\t\t\t5, flags | VARIANT_FLAG_HAS_SUPER_SAMPLE_REFERENCE_BIT);\n\t\t\t\t\t\t\t\tcmd->extract_pipeline_state(deferred);\n\t\t\t\t\t\t\t\ttasks.push_back(deferred);\n\t\t\t\t\t\t\t\t// Single samples of a pass that samples super-sampled textures.\n\t\t\t\t\t\t\t\tcmd->set_specialization_constant(\n\t\t\t\t\t\t\t\t\t5, flags | VARIANT_FLAG_HAS_SUPER_SAMPLE_REFERENCE_BIT | VARIANT_FLAG_HAS_TEXTURE_ARRAY_BIT);\n\t\t\t\t\t\t\t\tcmd->extract_pipeline_state(deferred);\n\t\t\t\t\t\t\t\ttasks.push_back(deferred);\n")
parallel_gs_replace("\t\t\t// If device has wave64, we always use it.\n\t\t\tif (device->supports_subgroup_size_log2(true, 6, 6))\n\t\t\t\tbreak;\n\t\t}\n"
    "\t\t}\n\t\tbdr_ubershader_end = tasks.size();\n")
parallel_gs_replace("\t\tcmd->set_specialization_constant_mask(0x7);\n\n\t\tstatic const struct { uint32_t psm; uint32_t cpsm; } formats[] = {\n"
    "\t\tcmd->set_specialization_constant_mask(0xf);\n\n\t\tstatic const struct { uint32_t psm; uint32_t cpsm; } formats[] = {\n")
parallel_gs_replace("\t\t\t\tfor (auto *prog : shaders.upload)\n\t\t\t\t{\n\t\t\t\t\tcmd->set_program(prog);\n\t\t\t\t\tcmd->extract_pipeline_state(deferred);\n\t\t\t\t\ttasks.push_back(deferred);\n\t\t\t\t}\n"
    "\t\t\t\tfor (auto *prog : shaders.upload)\n\t\t\t\t{\n\t\t\t\t\tcmd->set_program(prog);\n\t\t\t\t\tfor (uint32_t indirection = 0; indirection < 2; indirection++)\n\t\t\t\t\t{\n\t\t\t\t\t\tcmd->set_specialization_constant(3, indirection);\n\t\t\t\t\t\tcmd->extract_pipeline_state(deferred);\n\t\t\t\t\t\ttasks.push_back(deferred);\n\t\t\t\t\t}\n\t\t\t\t}\n")
parallel_gs_replace("\tsize_t num_tasks = tasks.size();\n\tsize_t target_threads = (std::thread::hardware_concurrency() + 1) / 2;\n\tsize_t tasks_per_thread = num_tasks / target_threads;\n\n\tfor (size_t thread_index = 0; thread_index < target_threads; thread_index++)\n\t{\n\t\tstd::vector<Vulkan::DeferredPipelineCompile> deferred(\n\t\t\ttasks.data() + thread_index * tasks_per_thread,\n\t\t\ttasks.data() + std::min<size_t>((thread_index + 1) * tasks_per_thread, tasks.size())\n\t\t);\n"
    "\t// The copy, upload and binning pipelines first (they are needed at once), each key once (with\n\t// wave64 every rate has the same subgroup size); each thread takes every target_threads-th task,\n\t// so that they all follow that order.\n\tstd::rotate(tasks.begin(), tasks.begin() + bdr_ubershader_end, tasks.end());\n\tstd::unordered_set<Util::Hash> seen;\n\ttasks.erase(std::remove_if(tasks.begin(), tasks.end(), [&](const Vulkan::DeferredPipelineCompile &task) {\n\t\treturn !seen.insert(task.hash).second;\n\t}), tasks.end());\n\tsize_t target_threads = std::max<size_t>(1, (std::thread::hardware_concurrency() + 1) / 2);\n\n\tfor (size_t thread_index = 0; thread_index < target_threads; thread_index++)\n\t{\n\t\tstd::vector<Vulkan::DeferredPipelineCompile> deferred;\n\t\tfor (size_t i = thread_index; i < tasks.size(); i += target_threads)\n\t\t\tdeferred.push_back(tasks[i]);\n")
parallel_gs_replace("\tinit_luts();\n\n\tkick_compilation_tasks();\n"
    "\tinit_luts();\n\n\tbdr_precompile_options = options;\n\tkick_compilation_tasks();\n")
parallel_gs_use_patched(${patch_file} gs_renderer_patched.cpp)
target_include_directories(parallel-gs PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src/lib/gs/shaders")
