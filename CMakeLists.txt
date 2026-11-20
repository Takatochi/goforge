cmake_minimum_required(VERSION 3.20)
project(goforge VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(GOFORGE_IS_TOP_LEVEL ON)
else()
    set(GOFORGE_IS_TOP_LEVEL OFF)
endif()

option(GOFORGE_BUILD_TESTS "Build goforge unit and integration tests" ${GOFORGE_IS_TOP_LEVEL})
option(GOFORGE_BUILD_EXAMPLES "Build goforge example applications" ${GOFORGE_IS_TOP_LEVEL})
option(GOFORGE_ENABLE_ASIO "Enable standalone Asio for non-blocking HTTP networking" ON)
set(GOFORGE_LOCAL_GORUN_DIR "" CACHE PATH "Optional local path to gorun repository (auto-detects ../Rutine if present)")

# ----------------------------------------------------------------------------
# Dependency 1: gorun (M:N Green Threads, Channels, Context, Defer, Sync)
# ----------------------------------------------------------------------------
if(NOT TARGET gorun::gorun)
    set(GORUN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GORUN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GORUN_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(GORUN_INSTALL OFF CACHE BOOL "" FORCE)

    set(_local_gorun "${GOFORGE_LOCAL_GORUN_DIR}")
    if(NOT _local_gorun AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Rutine/CMakeLists.txt")
        set(_local_gorun "${CMAKE_CURRENT_SOURCE_DIR}/../Rutine")
    endif()

    if(_local_gorun AND EXISTS "${_local_gorun}/CMakeLists.txt")
        message(STATUS "[goforge] Using local gorun runtime at: ${_local_gorun}")
        add_subdirectory("${_local_gorun}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/gorun-build" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "[goforge] Fetching gorun v0.3.0-alpha via FetchContent...")
        FetchContent_Declare(
            gorun
            GIT_REPOSITORY https://github.com/Takatochi/gorun.git
            GIT_TAG        v0.3.0-alpha
        )
        FetchContent_MakeAvailable(gorun)
    endif()
endif()

# ----------------------------------------------------------------------------
# Dependency 2: Standalone Asio (header-only non-blocking I/O reactor)
# ----------------------------------------------------------------------------
if(GOFORGE_ENABLE_ASIO)
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-32-0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(asio)

    add_library(goforge_asio INTERFACE)
    target_include_directories(goforge_asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
    target_compile_definitions(goforge_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    if(WIN32)
        target_compile_definitions(goforge_asio INTERFACE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(goforge_asio INTERFACE ws2_32 mswsock)
    endif()
endif()

# ----------------------------------------------------------------------------
# goforge Core Library (Header-Only C++20/23)
# ----------------------------------------------------------------------------
add_library(goforge INTERFACE)
add_library(goforge::goforge ALIAS goforge)

target_compile_features(goforge INTERFACE cxx_std_20)
target_include_directories(goforge INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(goforge INTERFACE gorun::gorun)

if(GOFORGE_ENABLE_ASIO)
    target_link_libraries(goforge INTERFACE goforge_asio)
    target_compile_definitions(goforge INTERFACE GOFORGE_HAS_ASIO=1)
endif()

if(MSVC)
    target_compile_options(goforge INTERFACE /W4 /permissive- /EHsc)
else()
    target_compile_options(goforge INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ----------------------------------------------------------------------------
# Tests & Examples
# ----------------------------------------------------------------------------
if(GOFORGE_BUILD_TESTS)
    enable_testing()

    add_executable(goforge_stage1_tests tests/test_stage1_di.cpp)
    target_link_libraries(goforge_stage1_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage1_tests COMMAND goforge_stage1_tests)

    if(GOFORGE_ENABLE_ASIO)
        add_executable(goforge_stage2_tests tests/test_stage2_http.cpp)
        target_link_libraries(goforge_stage2_tests PRIVATE goforge::goforge)
        add_test(NAME goforge_stage2_tests COMMAND goforge_stage2_tests)
    endif()
endif()

if(GOFORGE_BUILD_EXAMPLES)
    add_executable(goforge_example_01_di examples/01_di_and_lifecycle.cpp)
    target_link_libraries(goforge_example_01_di PRIVATE goforge::goforge)

    if(GOFORGE_ENABLE_ASIO)
        add_executable(goforge_example_02_http examples/02_http_server.cpp)
        target_link_libraries(goforge_example_02_http PRIVATE goforge::goforge)
    endif()
endif()
