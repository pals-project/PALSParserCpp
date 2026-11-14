cmake_minimum_required(VERSION 3.16)
project(PALSParserCpp)

# Default to Release when no build type is given. This is not just about
# optimization: an empty build type leaves NDEBUG undefined, and c4core (vendored
# by rapidyaml) then compiles a debugbreak path calling __builtin_debugtrap,
# which is clang-only and fails to build under GCC. Multi-config generators
# ignore CMAKE_BUILD_TYPE, so skip them rather than set a variable they discard.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
               STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()
include(FetchContent)

FetchContent_Declare(
  rapidyaml
  GIT_REPOSITORY https://github.com/biojppm/rapidyaml.git
  GIT_TAG v0.11.1
  GIT_SHALLOW FALSE
)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)

# PCRE2 provides the PCRE2 regex syntax used by the {beamline} and {element}
# components of lattice parameter name matching (see match_lattice_parameters).
FetchContent_Declare(
  pcre2
  GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
  GIT_TAG pcre2-10.44
)
# Build only the 8-bit library, statically, and skip the tooling/tests so the
# regex code is embedded directly into libPALSParserCpp.
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT OFF CACHE BOOL "" FORCE)
set(PCRE2_STATIC_PIC ON CACHE BOOL "" FORCE)

# AtomicAndPhysicalConstantsCLib (apc) provides the particle-data functions
# (mass_of / charge_of / anomalous_moment_of) and the named physical constants
# used by the PALS expression evaluator. Its values mirror
# AtomicAndPhysicalConstants.jl so the whole ecosystem shares one set of numbers.
# Pinned to a tag, not main: the reference values in the tests are tied to a
# specific APC release, so an upstream push must not silently change them here.
# apc's version tracks the APC release it mirrors (v0.11.1 = APC v0.11.1).
FetchContent_Declare(
  apc
  GIT_REPOSITORY https://github.com/pals-project/AtomicAndPhysicalConstantsCLib.git
  GIT_TAG v0.11.1
)
set(APC_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(rapidyaml)
FetchContent_MakeAvailable(Catch2)
FetchContent_MakeAvailable(pcre2)
FetchContent_MakeAvailable(apc)

file(COPY ${CMAKE_SOURCE_DIR}/lattice_files/
     DESTINATION ${CMAKE_BINARY_DIR}/lattice_files/)

add_library(PALSParserCpp SHARED
  src/PALSParserCpp.cpp
  src/pals_expand.cpp
  src/pals_combine.cpp
  src/pals_structure.cpp
  src/pals_evaluate.cpp
  src/pals_controllers.cpp
  src/pals_sets.cpp
  src/pals_bookkeeper.cpp
  src/pals_tree.cpp
  src/pals_match.cpp
  src/pals_util.cpp
  src/pals_expression.cpp
  src/pals_floor.cpp
  src/pals_check.cpp)
target_link_libraries(PALSParserCpp PUBLIC ryml PRIVATE pcre2-8-static apc)

add_executable(example_rw examples/example_rw.cpp)
target_link_libraries(example_rw PALSParserCpp ryml)

add_executable(print_lattices examples/print_lattices.cpp)
target_link_libraries(print_lattices PALSParserCpp ryml)

# Must come after PALSParserCpp is defined: tests/CMakeLists.txt links it.
add_subdirectory(tests)
