# Integrate locally regenerated guest C++; never ship a dummy playable target.
option(BDR_BUILD_GAME "Link the generated Burnout guest functions" OFF)
set(BDR_UNITY_BATCH_SIZE 64 CACHE STRING "Generated functions per unity translation unit")
set(BDR_COMPILER_WORKERS 8 CACHE STRING "MSVC parallel compiler workers")
if(BDR_BUILD_GAME)
    if(NOT TARGET ps2EntryRunner)
        message(FATAL_ERROR "BDR_BUILD_GAME requires PS2X_BUILD_RUNTIME=ON")
    endif()
    set(BDR_GENERATED_DIR "${PROJECT_SOURCE_DIR}/generated" CACHE PATH "Local translated game source")
    if(NOT EXISTS "${BDR_GENERATED_DIR}/register_functions.cpp" OR
       NOT EXISTS "${BDR_GENERATED_DIR}/ps2_recompiled_functions.h")
        message(FATAL_ERROR "Generate guest sources first: python tools/project.py generate --tool <ps2_recomp> --function-map <csv>")
    endif()
    file(GLOB BDR_GENERATED_CPP CONFIGURE_DEPENDS "${BDR_GENERATED_DIR}/*.cpp")
    # Replace upstream's default runner with a runner that mounts the original ISO.
    get_target_property(BDR_RUNNER_SOURCES ps2EntryRunner SOURCES)
    list(FILTER BDR_RUNNER_SOURCES EXCLUDE REGEX "(^|/)src/main\\.cpp$")
    list(FILTER BDR_RUNNER_SOURCES EXCLUDE REGEX "(^|/)src/runner/register_functions\\.cpp$")
    set_property(TARGET ps2EntryRunner PROPERTY SOURCES "${BDR_RUNNER_SOURCES}")
    set(BDR_PROJECT_SOURCES
        "${PROJECT_SOURCE_DIR}/src/burnout_main.cpp"
        "${PROJECT_SOURCE_DIR}/src/burnout_overrides.cpp")
    # Keep hand-written sources out of the generated unity batches so editing
    # them recompiles one file instead of a batch of guest functions.
    set_source_files_properties(${BDR_PROJECT_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_sources(ps2EntryRunner PRIVATE ${BDR_GENERATED_CPP} ${BDR_PROJECT_SOURCES})
    target_include_directories(ps2EntryRunner PRIVATE "${BDR_GENERATED_DIR}")
    if(PS2X_ENABLE_RUNNER_PCH)
        target_precompile_headers(ps2EntryRunner PRIVATE
            "${BDR_GENERATED_DIR}/ps2_recompiled_functions.h"
            "${BDR_GENERATED_DIR}/ps2_recompiled_stubs.h")
    endif()
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME burnout_dominator)
    # Unity builds amortize heavy R5900 headers across generated functions.
    set_target_properties(ps2EntryRunner PROPERTIES UNITY_BUILD_BATCH_SIZE "${BDR_UNITY_BATCH_SIZE}")
    if(MSVC)
        target_compile_options(ps2EntryRunner PRIVATE /bigobj /utf-8 "/MP${BDR_COMPILER_WORKERS}")
    endif()
endif()

# Checks the Burnout bindings and ROM0 profile without generated guest code.
if(TARGET ps2_runtime AND TARGET ps2_test_function_table)
    add_executable(burnout_overrides_tests
        "${PROJECT_SOURCE_DIR}/tests/native/burnout_overrides_tests.cpp"
        "${PROJECT_SOURCE_DIR}/src/burnout_overrides.cpp"
        $<TARGET_OBJECTS:ps2_test_function_table>)
    target_include_directories(burnout_overrides_tests PRIVATE "${PROJECT_SOURCE_DIR}/ps2xRuntime/include")
    target_link_libraries(burnout_overrides_tests PRIVATE ps2_runtime)
    if(MSVC)
        target_compile_options(burnout_overrides_tests PRIVATE /utf-8)
    endif()
    if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
        ps2x_stage_ffmpeg_runtime_dlls(burnout_overrides_tests)
    endif()
    # Same optimization settings as ps2x_tests: with LTO on, ps2_runtime holds
    # IPO objects (LLVM bitcode with Clang) that a non-IPO link cannot read.
    # ReleaseMode.cmake sets IPO_SUPPORTED in the including scope.
    include("${PROJECT_SOURCE_DIR}/ps2xRuntime/cmake/ReleaseMode.cmake")
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        EnableFastReleaseMode(burnout_overrides_tests)
    endif()
endif()
