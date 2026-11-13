# paraLLEl-GS (Arntzen Software, LGPLv3): a GS emulated with Vulkan compute.
# Pinned one commit after the one upstream PS2Recomp pull request #285 used: cc6184a fixes the
# sampler feedback shader that crashed AMD's Windows shader compiler.
# FETCHCONTENT_SOURCE_DIR_PARALLEL_GS can point at a local checkout.
include(FetchContent)
set(PARALLEL_GS_STANDALONE ON CACHE BOOL "Build parallel-gs as a library" FORCE)
set(GRANITE_TOOLS OFF CACHE BOOL "" FORCE)
set(GRANITE_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)

# parallel-gs defaults to the static CRT; the runtime links the DLL one.
if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

FetchContent_Declare(parallel_gs
    GIT_REPOSITORY https://github.com/Arntzen-Software/parallel-gs.git
    GIT_TAG cc6184af7e0c03da603045ca371ffa5dae9b0655
    GIT_SUBMODULES_RECURSE TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(parallel_gs)

include("${CMAKE_CURRENT_LIST_DIR}/ParallelGSPatches.cmake")
if(MSVC)
    # Debug info in the PDB, so profiles name paraLLEl-GS code too.
    target_compile_options(parallel-gs PRIVATE $<$<CONFIG:Release>:/Z7>)
endif()
target_sources(ps2_runtime PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src/lib/gs/gs_parallel_backend.cpp"
                                   "${CMAKE_CURRENT_LIST_DIR}/../src/lib/gs/gs_async_backend.cpp")
target_link_libraries(ps2_runtime PRIVATE parallel-gs)
target_compile_definitions(ps2_runtime PUBLIC PS2X_GS_PARALLEL=1)
install(FILES "${parallel_gs_SOURCE_DIR}/COPYING.LGPLv3" DESTINATION share/licenses/parallel-gs)
