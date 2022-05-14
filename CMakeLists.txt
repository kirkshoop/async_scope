cmake_minimum_required(VERSION 3.17.0)

# Define the project
project(async_scope-dev LANGUAGES CXX)

if (EXISTS ${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
    include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
    conan_basic_setup(TARGETS)
else ()
    message(ERROR "The file conanbuildinfo.cmake doesn't exist, you have to run conan install first")
endif ()

# Set the version of the project
project(async_scope VERSION "${CONAN_PACKAGE_VERSION}")

# Print CMake configuration
message(STATUS "System           : ${CMAKE_SYSTEM}")
message(STATUS "System name      : ${CMAKE_SYSTEM_NAME}")
message(STATUS "System ver       : ${CMAKE_SYSTEM_VERSION}")
message(STATUS "Conan compiler   : ${CONAN_SETTINGS_COMPILER} ${CONAN_SETTINGS_COMPILER_VERSION}")
message(STATUS "Conan libcxx     : ${CONAN_SETTINGS_COMPILER_LIBCXX}")
message(STATUS)

# The main library
add_library(async_scope INTERFACE)

# Set the version and current build date
set(async_scope_VERSION "${PROJECT_VERSION}")
set(async_scope_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
string(TIMESTAMP async_scope_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP async_scope_BUILD_YEAR "%Y")

message(STATUS "Library ver      : ${async_scope_VERSION}")
message(STATUS "Build date       : ${async_scope_BUILD_DATE}")
message(STATUS "Build year       : ${async_scope_BUILD_YEAR}")
message(STATUS)

# The include directories for async_scope
target_include_directories(async_scope INTERFACE
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>
                           $<INSTALL_INTERFACE:include>
                           ${CMAKE_CURRENT_SOURCE_DIR}/external/p2300/include/
                           ${CMAKE_CURRENT_SOURCE_DIR}/external/p2300/examples/
                           )

# Ensure that we link with the threading library
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
find_package(Threads REQUIRED)
target_link_libraries(async_scope INTERFACE Threads::Threads)

# Use C++20 standard
target_compile_features(async_scope INTERFACE cxx_std_20)

# Turn all warnings
target_compile_options(async_scope INTERFACE
                       $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
                       -Wall>
                       $<$<CXX_COMPILER_ID:MSVC>:
                       /W4>)
# template backtrace limit
target_compile_options(async_scope INTERFACE
                       $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>:
                       -ftemplate-backtrace-limit=0>
                       )
# Proper use of libc++
if (CONAN_SETTINGS_COMPILER_LIBCXX STREQUAL "libc++")
    target_compile_options(async_scope INTERFACE -stdlib=libc++)
    target_link_options(async_scope INTERFACE -lc++)
endif ()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_compile_options(-fdiagnostics-color=always)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    add_compile_options(-fcolor-diagnostics)
endif ()

add_library(async_scope::async_scope ALIAS async_scope)

# Now, set up test executable
enable_testing()

set(test_sourceFiles
    test/test_main.cpp
    test/test_dtor.cpp
    test/test_spawn.cpp
    test/test_spawn_future.cpp
    )

add_executable(test.async_scope ${test_sourceFiles})

target_include_directories(test.async_scope PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test.async_scope async_scope)
target_link_libraries(test.async_scope CONAN_PKG::catch2)

# Discover the Catch2 test built by the application
include(CTest)
include(Catch)
catch_discover_tests(test.async_scope)

# Set up examples
function(def_example target sourceFile)
    add_executable(${target} ${sourceFile})
    target_link_libraries(${target} async_scope)
endfunction()

def_example(example.scope "examples/scope.cpp")
