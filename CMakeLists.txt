cmake_minimum_required(VERSION 3.20)
project(goforge VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(GOFORGE_IS_TOP_LEVEL ON)
else()
    set(GOFORGE_IS_TOP_LEVEL OFF)
endif()

option(GOFORGE_BUILD_TESTS "Build goforge unit and integration tests" ${GOFORGE_IS_TOP_LEVEL})
option(GOFORGE_BUILD_EXAMPLES "Build goforge example applications" ${GOFORGE_IS_TOP_LEVEL})
option(GOFORGE_BUILD_BENCHMARKS "Build goforge HTTP performance benchmarks" ${GOFORGE_IS_TOP_LEVEL})
option(GOFORGE_ENABLE_SQLITE "Enable embedded SQLite 3 driver for goforge::data ORM" ON)
option(GOFORGE_INSTALL "Generate install target and CMake package config for goforge (for vcpkg/Conan/system install)" OFF)
set(GOFORGE_LOCAL_GORUN_DIR "" CACHE PATH "Optional local path to gorun repository (auto-detects ../Rutine if present)")

# ----------------------------------------------------------------------------
# Dependency 1: gorun (M:N Green Threads, Channels, Native IOCP/epoll/kqueue NetPoller)
# ----------------------------------------------------------------------------
if(NOT TARGET gorun::gorun)
    find_package(gorun CONFIG QUIET)
endif()
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
        message(STATUS "[goforge] Fetching gorun (v0.4.0) via FetchContent...")
        FetchContent_Declare(
            gorun
            GIT_REPOSITORY https://github.com/Takatochi/gorun.git
            GIT_TAG        v0.4.0
        )
        FetchContent_MakeAvailable(gorun)
    endif()
endif()

# ----------------------------------------------------------------------------
# Dependency 2: nlohmann/json (Header-only JSON Serialization & DTO Binding)
# ----------------------------------------------------------------------------
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ----------------------------------------------------------------------------
# Dependency 3: SQLite 3 (Optional embedded SQL backend for goforge::data ORM)
# ----------------------------------------------------------------------------
if(GOFORGE_ENABLE_SQLITE)
    find_package(SQLite3 QUIET)
    if(NOT SQLite3_FOUND)
        message(STATUS "[goforge] Fetching embedded SQLite 3 amalgamation via Git...")
        set(SQLITECPP_INTERNAL_SQLITE ON CACHE BOOL "" FORCE)
        set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
        set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
        set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            sqlite3_src
            GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
            GIT_TAG        3.3.3
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  sqlite3
        )
        FetchContent_MakeAvailable(sqlite3_src)
    endif()
endif()

# ----------------------------------------------------------------------------
# goforge Core Library (Header-Only C++20/23 — 100% Native gorun Networking!)
# ----------------------------------------------------------------------------
add_library(goforge INTERFACE)
add_library(goforge::goforge ALIAS goforge)

target_compile_features(goforge INTERFACE cxx_std_20)
target_include_directories(goforge INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(goforge INTERFACE gorun::gorun nlohmann_json::nlohmann_json)

if(GOFORGE_ENABLE_SQLITE)
    if(TARGET SQLite::SQLite3)
        target_link_libraries(goforge INTERFACE SQLite::SQLite3)
    elseif(TARGET sqlite3)
        target_link_libraries(goforge INTERFACE sqlite3)
    endif()
    target_compile_definitions(goforge INTERFACE GOFORGE_HAS_SQLITE=1)
endif()

if(MSVC)
    target_compile_options(goforge INTERFACE /W4 /permissive- /EHsc)
else()
    target_compile_options(goforge INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ----------------------------------------------------------------------------
# Tests, Examples & Benchmarks
# ----------------------------------------------------------------------------
if(GOFORGE_BUILD_TESTS)
    enable_testing()

    add_executable(goforge_stage1_tests tests/test_stage1_di.cpp)
    target_link_libraries(goforge_stage1_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage1_tests COMMAND goforge_stage1_tests)

    add_executable(goforge_stage2_tests tests/test_stage2_http.cpp)
    target_link_libraries(goforge_stage2_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage2_tests COMMAND goforge_stage2_tests)

    add_executable(goforge_stage3_tests tests/test_stage3_web.cpp)
    target_link_libraries(goforge_stage3_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage3_tests COMMAND goforge_stage3_tests)

    add_executable(goforge_stage4_tests tests/test_stage4_prod.cpp)
    target_link_libraries(goforge_stage4_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage4_tests COMMAND goforge_stage4_tests)

    add_executable(goforge_stage5_json_tests tests/test_stage5_json.cpp)
    target_link_libraries(goforge_stage5_json_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage5_json_tests COMMAND goforge_stage5_json_tests)

    add_executable(goforge_stage6_ergonomics_query_tests tests/test_stage6_ergonomics_query.cpp)
    target_link_libraries(goforge_stage6_ergonomics_query_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage6_ergonomics_query_tests COMMAND goforge_stage6_ergonomics_query_tests)

    add_executable(goforge_stage7_orm_tests tests/test_stage7_orm_data.cpp)
    target_link_libraries(goforge_stage7_orm_tests PRIVATE goforge::goforge)
    add_test(NAME goforge_stage7_orm_tests COMMAND goforge_stage7_orm_tests)
endif()

if(GOFORGE_BUILD_EXAMPLES)
    add_executable(goforge_example_01_di examples/01_di_and_lifecycle.cpp)
    target_link_libraries(goforge_example_01_di PRIVATE goforge::goforge)

    add_executable(goforge_example_02_http examples/02_http_server.cpp)
    target_link_libraries(goforge_example_02_http PRIVATE goforge::goforge)

    add_executable(goforge_example_03_rest examples/03_rest_api_gin_fx.cpp)
    target_link_libraries(goforge_example_03_rest PRIVATE goforge::goforge)

    add_executable(goforge_example_04_ergonomic examples/04_ergonomic_webapp_and_query.cpp)
    target_link_libraries(goforge_example_04_ergonomic PRIVATE goforge::goforge)

    if(GOFORGE_ENABLE_SQLITE)
        add_executable(goforge_example_05_orm_rest examples/05_orm_sqlite_rest_api.cpp)
        target_link_libraries(goforge_example_05_orm_rest PRIVATE goforge::goforge)
    endif()
endif()

if(GOFORGE_BUILD_BENCHMARKS)
    add_executable(bench_goforge_http benchmarks/bench_goforge_http.cpp)
    target_link_libraries(bench_goforge_http PRIVATE goforge::goforge)

    add_executable(bench_std_http benchmarks/bench_std_http.cpp)
    target_link_libraries(bench_std_http PRIVATE goforge::goforge)
endif()

# ----------------------------------------------------------------------------
# CMake Package Installation & Export (supports vcpkg, Conan 2.0, find_package)
# ----------------------------------------------------------------------------
if(GOFORGE_INSTALL)
    install(TARGETS goforge
        EXPORT goforgeTargets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY include/goforge
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(FILES LICENSE README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
    )

    install(EXPORT goforgeTargets
        FILE goforgeTargets.cmake
        NAMESPACE goforge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/goforge
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/goforgeConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/goforgeConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/goforge
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/goforgeConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/goforgeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/goforgeConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/goforge
    )
endif()
