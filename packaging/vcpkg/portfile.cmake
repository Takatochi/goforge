vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Takatochi/goforge
    REF "v${VERSION}"
    SHA512 69b76565bb90d1778056c0e3820d92c848a7a36fc3c7e50fcbba6dd90e0b84795f90991b3e5af9dd21fa84848d49978e3255b6600628256f33498d7315097f1d
    HEAD_REF main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        sqlite GOFORGE_ENABLE_SQLITE
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DGOFORGE_BUILD_TESTS=OFF
        -DGOFORGE_BUILD_EXAMPLES=OFF
        -DGOFORGE_BUILD_BENCHMARKS=OFF
        -DGOFORGE_INSTALL=ON
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/goforge)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug" "${CURRENT_PACKAGES_DIR}/lib")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
