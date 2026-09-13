# rapidproto is a HOST code-generator tool plus the header-only runtime it emits; it has no
# third-party dependencies. Consumers invoke rapidprotoc at build time via rapidproto_generate(), so
# a cross-compiling consumer depends on this port with "host": true. Release-only: the runtime is
# header-only and the tool is a standalone host binary, so a debug build buys nothing.
set(VCPKG_BUILD_TYPE release)

# SHA512 is a PLACEHOLDER (currently v0.4.0's) -- GitHub generates the v0.5.0 source tarball only at
# tag time, so recompute against the real REF before this port is used:
#   vcpkg_from_github(... REF v0.5.0 ...) with SHA512 0  then paste the "Actual hash" it prints.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO VeaaC/rapidproto
    REF "v${VERSION}"
    SHA512 e8566ad772c2dd6d8ab0a3fe62bbdaa2d3ed609891f907362f7d49f7cab30e4d2d220d2789a276d7bcfe04623542968960d835412140f3af43e412e7f5621a0d
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRAPIDPROTO_INSTALL=ON
        -DRAPIDPROTO_BUILD_TESTS=OFF
)
vcpkg_cmake_install()

# config_fixup relocates the exported rapidproto::rapidprotoc imported target to tools/<port>/ (its
# convention for an executable); vcpkg_copy_tools then moves the actual binary there so the two
# agree. AUTO_CLEAN removes the now-empty bin/.
vcpkg_cmake_config_fixup(PACKAGE_NAME rapidproto CONFIG_PATH lib/cmake/rapidproto)
vcpkg_copy_tools(TOOL_NAMES rapidprotoc AUTO_CLEAN)

# Header-only runtime: no libraries to keep.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib" "${CURRENT_PACKAGES_DIR}/debug")

vcpkg_install_copyright(FILE_LIST
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/NOTICE"
    "${SOURCE_PATH}/THIRD_PARTY_NOTICES.md")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
