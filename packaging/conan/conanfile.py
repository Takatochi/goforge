from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.files import copy, get
from conan.tools.layout import basic_layout
import os

required_conan_version = ">=2.0"


class GoforgeConan(ConanFile):
    name = "goforge"
    version = "0.4.0"
    description = (
        "Header-only C++20/23 web, Uber Fx-style DI, Gin-style router, fluent Query AST, "
        "and EF-style ORM framework built 100% natively on gorun."
    )
    license = "Apache-2.0"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/Takatochi/goforge"
    topics = ("web", "http", "di", "orm", "linq", "rest", "green-threads", "header-only", "cpp20")
    package_type = "header-library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "with_sqlite": [True, False],
    }
    default_options = {
        "with_sqlite": True,
    }
    no_copy_source = True

    def layout(self):
        basic_layout(self, src_folder="src")

    def requirements(self):
        self.requires("gorun/0.4.0")
        self.requires("nlohmann_json/3.11.3")
        if self.options.with_sqlite:
            self.requires("sqlite3/3.46.1")

    def validate(self):
        check_min_cppstd(self, 20)

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def package(self):
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        copy(
            self,
            "*.hpp",
            src=os.path.join(self.source_folder, "include"),
            dst=os.path.join(self.package_folder, "include"),
        )

    def package_id(self):
        self.info.clear()

    def package_info(self):
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.set_property("cmake_file_name", "goforge")
        self.cpp_info.set_property("cmake_target_name", "goforge::goforge")
        if self.options.with_sqlite:
            self.cpp_info.defines.append("GOFORGE_HAS_SQLITE=1")
