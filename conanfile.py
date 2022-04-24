from conans import ConanFile, CMake, tools
import re
from os import path

class AsyncScopeRecipe(ConanFile):
   name = "async_scope"
   description = "reference implementation for async_scope proposal (P2519)"
   author = "Kirk Shoop, Lee Howes, Lucian Radu Teodorescu"
   topics = ("C++", "concurrency")
   homepage = "https://github.com/kirkshoop/async_scope"
   url = "https://github.com/kirkshoop/async_scope"
   license = "MIT License"

   settings = "os", "compiler", "build_type", "arch"
   generators = "cmake"
   build_policy = "missing"   # Some of the dependencies don't have builds for all our targets

   options = {"shared": [True, False], "fPIC": [True, False], "with_profiling": [True, False]}
   default_options = {"shared": False, "fPIC": True, "with_profiling": False}

   exports_sources = ("include/*", "CMakeLists.txt")

   def set_version(self):
      # Get the version from the spec file
      content = tools.load(path.join(self.recipe_folder, "asyncscope.md"))
      rev = re.search(r"document: D2519R(\d+)", content).group(1).strip()
      self.version = f"0.{rev}.0"

   def build_requirements(self):
      self.build_requires("catch2/2.13.6")

   def config_options(self):
       if self.settings.os == "Windows":
           del self.options.fPIC

   def build(self):
      # Note: options "shared" and "fPIC" are automatically handled in CMake
      cmake = self._configure_cmake()
      cmake.build()

   def package(self):
      cmake = self._configure_cmake()
      cmake.install()

   def package_info(self):
      self.cpp_info.libs = self.collect_libs()

   def _configure_cmake(self):
      cmake = CMake(self)
      if self.settings.compiler == "Visual Studio" and self.options.shared:
         cmake.definitions["CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS"] = True
      cmake.configure(source_folder=None)
      return cmake
