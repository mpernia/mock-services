from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class MockServicesConan(ConanFile):
    name = "mock-services"
    version = "0.1.0"
    package_type = "library"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_sftp": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_sftp": False,
    }

    generators = "CMakeToolchain", "CMakeDeps"

    exports_sources = (
        "CMakeLists.txt", "mock-services-config.cmake.in", "include/*", "src/*",
        "test/*", "examples/*",
        "third_party/CMakeLists.txt", "third_party/cmake/*"
    )

    def requirements(self):
        self.requires("cpp-httplib/0.18.0")
        self.requires("pugixml/1.15")

    def build_requirements(self):
        self.test_requires("gtest/1.10.0")

    def validate(self):
        if self.settings.os == "Windows" and self.settings.compiler == "gcc":
            pass  # MinGW is supported

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        variables = {"MOCK_SERVICES_BUILD_EXAMPLES": False}
        if self.options.with_sftp:
            variables["MOCK_SERVICES_ENABLE_SFTP"] = "ON"
        cmake.configure(variables=variables)
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["mock-services"]
        if self.settings.os == "Windows":
            self.cpp_info.system_libs = ["ws2_32", "cryptui"]

        # libssh + mbedtls are downloaded at configure-time via FetchContent
        # (see third_party/CMakeLists.txt). They are only compiled when with_sftp=True.
        # Platform system libs are handled by libssh's CMake.
        if self.options.with_sftp:
            if self.settings.os == "Linux":
                self.cpp_info.system_libs.append("pthread")

        self.cpp_info.set_property("cmake_target_name", "mock-services::mock-services")
        self.cpp_info.set_property("cmake_file_name", "mock-services")
