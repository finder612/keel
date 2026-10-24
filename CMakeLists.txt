cmake_minimum_required(VERSION 3.16)

project(keel
    VERSION 1.3.0
    DESCRIPTION "Resilience primitives (retry, retry budgets, circuit breaker, fallback, timeout, rate limiting, single-flight) for C++ backends"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# The library is header-only: an INTERFACE target carrying the include
# directory and the C++17 requirement.
# ---------------------------------------------------------------------------
add_library(keel INTERFACE)
add_library(keel::keel ALIAS keel)

target_include_directories(keel INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(keel INTERFACE cxx_std_17)

# Threads are used by the default sleeper and by the circuit breaker's mutex.
find_package(Threads REQUIRED)
target_link_libraries(keel INTERFACE Threads::Threads)

# Only build tests/examples when keel is the top-level project, so that
# add_subdirectory() / FetchContent consumers stay lean.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(KEEL_BUILD_TESTS "Build the keel test suite" ON)
    option(KEEL_BUILD_EXAMPLES "Build the keel examples" ON)
else()
    option(KEEL_BUILD_TESTS "Build the keel test suite" OFF)
    option(KEEL_BUILD_EXAMPLES "Build the keel examples" OFF)
endif()

if(KEEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(KEEL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Installation: a header-only package consumers can find_package(keel).
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS keel EXPORT keelTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT keelTargets
    FILE keelTargets.cmake
    NAMESPACE keel::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/keel)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/keelConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/keelConfig.cmake.in"
"@PACKAGE_INIT@\ninclude(CMakeFindDependencyMacro)\nfind_dependency(Threads)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/keelTargets.cmake\")\n")
configure_package_config_file(
    "${CMAKE_CURRENT_BINARY_DIR}/keelConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/keelConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/keel)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/keelConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/keelConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/keel)
