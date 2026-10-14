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
