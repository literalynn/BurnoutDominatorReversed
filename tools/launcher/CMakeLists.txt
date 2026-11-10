# The launcher's window (launcher.cpp). On Windows it is BurnoutDominator.exe, with the first start's downloads
# (../portable/BurnoutDominator.cpp); elsewhere bdr_launcher, which BurnoutDominator.py builds and starts.
#   cmake -S tools/launcher -B <build> && cmake --build <build> --config Release
cmake_minimum_required(VERSION 3.20)
# raylib asks for an older CMake: the static runtime below needs the newer behaviour in its project too.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
project(BurnoutDominatorLauncher LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # one file to hand out: no runtime to install

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git GIT_TAG 5.5 GIT_SHALLOW TRUE) # the game's
FetchContent_MakeAvailable(raylib)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SOURCES launcher.cpp system.cpp ${ROOT}/ps2xRuntime/src/lib/host_settings.cpp) # the key names of settings.ini
if(WIN32)
    enable_language(RC)
    add_executable(launcher WIN32 ${SOURCES} ../portable/BurnoutDominator.cpp ../portable/BurnoutDominator.rc)
    set_target_properties(launcher PROPERTIES OUTPUT_NAME BurnoutDominator)
    target_compile_definitions(launcher PRIVATE UNICODE _UNICODE)
    target_link_libraries(launcher PRIVATE bcrypt urlmon wininet ws2_32 dwmapi)
else()
    add_executable(launcher ${SOURCES})
    set_target_properties(launcher PROPERTIES OUTPUT_NAME bdr_launcher)
endif()
target_include_directories(launcher PRIVATE ${ROOT}/ps2xRuntime/include)
target_link_libraries(launcher PRIVATE raylib)
if(MSVC)
    target_compile_options(launcher PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(launcher PRIVATE _CRT_SECURE_NO_WARNINGS) # getenv
else()
    target_compile_options(launcher PRIVATE -Wall -Wextra)
endif()
