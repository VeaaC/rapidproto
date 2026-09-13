import os
import re

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load


class RapidprotoConan(ConanFile):
    name = "rapidproto"
    # A code-generator TOOL, nothing else to consume: generated code is self-contained (rapidprotoc
    # drops its own copy of the std-only runtime headers beside its output), so a consumer never links
    # or `requires` a runtime package -- it only `tool_requires` the generator and calls
    # rapidproto_generate() at build time. The runtime headers ship inside this package because the
    # tool emits them, not as a separate dependency. Hence one package, consumed via tool_requires
    # (build context) even when cross-compiling; the tool runs on the build host, the output is portable.
    package_type = "application"
    license = "Apache-2.0"
    homepage = "https://github.com/VeaaC/rapidproto"
    url = "https://github.com/VeaaC/rapidproto"
    description = (
        "A decode-only protobuf code generator for C++: turns .proto schemas into fast arena and "
        "streaming decoders that depend only on a std-library-only, header-only runtime."
    )
    topics = ("protobuf", "protocol-buffers", "code-generator", "decoder", "cpp17", "header-only")

    settings = "os", "arch", "compiler", "build_type"
    # No options: the runtime is header-only and the generator is a standalone executable -- there is
    # nothing to build shared, and no fPIC surface.

    # The generator's build inputs only (tests/, examples/, docs/ are not needed to build + install
    # rapidprotoc with RAPIDPROTO_BUILD_TESTS=OFF).
    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "src/*",
        "include/*",
        "wellknown/*",
        "LICENSE",
        "NOTICE",
        "THIRD_PARTY_NOTICES.md",
    )

    def set_version(self):
        # Single source of truth: project(rapidproto VERSION x.y.z) in CMakeLists.txt, so the recipe
        # version can never drift from the library's.
        text = load(self, os.path.join(self.recipe_folder, "CMakeLists.txt"))
        match = re.search(r"project\(rapidproto\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text)
        if not match:
            raise Exception("conanfile: could not read project(VERSION) from CMakeLists.txt")
        self.version = match.group(1)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["RAPIDPROTO_INSTALL"] = True
        tc.cache_variables["RAPIDPROTO_BUILD_TESTS"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build(target="rapidprotoc")

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        # Reuse the library's OWN installed CMake package (rapidprotoConfig.cmake + the imported
        # rapidproto::rapidprotoc target + the rapidproto_generate() helper), rather than letting
        # CMakeDeps generate a competing config that would define neither the executable target nor
        # the helper. cmake_find_mode=none suppresses that generation; putting the config dir on the
        # build path makes the consumer's find_package(rapidproto) resolve the shipped config.
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = [os.path.join("lib", "cmake", "rapidproto")]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.bindirs = ["bin"]
        self.cpp_info.libdirs = []  # header-only runtime; the executable is the only binary
