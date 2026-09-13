import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, cmake_layout


class TestPackageConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    # Only CMakeToolchain: rapidproto ships its own CMake config (find_package finds it via the
    # tool's builddirs on the prefix path), so no CMakeDeps.
    generators = "CMakeToolchain"

    def build_requirements(self):
        # rapidprotoc is a build-time host tool -- tool_requires, not requires.
        self.tool_requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            cmake = CMake(self)
            cmake.test()
