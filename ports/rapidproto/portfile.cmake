# rapidproto is a HOST code-generator tool plus the header-only runtime it emits; it has no
# third-party dependencies. Consumers invoke rapidprotoc at build time via rapidproto_generate(), so
# a cross-compiling consumer depends on this port with "host": true. Release-only: the runtime is
# header-only and the tool is a standalone host binary, so a debug build buys nothing.
set(VCPKG_BUILD_TYPE release)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO VeaaC/rapidproto
    REF "v${VERSION}"
    SHA512 4681bf52f91f9dd69ff72e7cb5b9846a977cdcf00d4679ada38b0cf3dead4a6ecf1b8af654613315ab493ab9ec6565441f234d782e7ae27c50ae9288419af3ce
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
