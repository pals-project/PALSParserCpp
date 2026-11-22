cmake_minimum_required(VERSION 3.18)
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

include(CTest)
include(GNUInstallDirs)
option(PALS_BUILD_EXAMPLES "Build command-line examples" ON)
option(PALS_USE_SYSTEM_DEPS "Use installed rapidyaml, PCRE2, APC and (when testing) Catch2 libraries" OFF)

option(PALS_PCRE2_STATIC "Installed PCRE2 library is static (system dependencies only)" OFF)

# Windows finds a DLL beside the executable that loads it, or on PATH, so the
# tests and examples must land in the same directory as PALSParserCpp.dll.
# Multi-configuration generators (Visual Studio) add a per-configuration
# subdirectory to it: build/Release, say. Other platforms keep the default
# layout, where the test binary is build/tests/tests.
if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

if(PALS_USE_SYSTEM_DEPS)
  find_package(ryml CONFIG REQUIRED)
  find_path(PALS_PCRE2_INCLUDE_DIR pcre2.h REQUIRED)
  find_library(PALS_PCRE2_LIBRARY NAMES pcre2-8 REQUIRED)
  find_path(PALS_APC_INCLUDE_DIR apc/apc.h REQUIRED)
  find_library(PALS_APC_LIBRARY NAMES apc REQUIRED)
  add_library(pals_pcre2 UNKNOWN IMPORTED)
  set_target_properties(pals_pcre2 PROPERTIES
    IMPORTED_LOCATION "${PALS_PCRE2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PALS_PCRE2_INCLUDE_DIR}")
  if(PALS_PCRE2_STATIC)
    set_property(TARGET pals_pcre2 PROPERTY INTERFACE_COMPILE_DEFINITIONS PCRE2_STATIC)
  endif()
  add_library(pals_apc UNKNOWN IMPORTED)
  set_target_properties(pals_apc PROPERTIES
    IMPORTED_LOCATION "${PALS_APC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PALS_APC_INCLUDE_DIR}")
  set(PALS_DEPENDENCIES ryml::ryml pals_pcre2 pals_apc)
  if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
  endif()
else()
  include(FetchContent)

  FetchContent_Declare(
    rapidyaml
    GIT_REPOSITORY https://github.com/biojppm/rapidyaml.git
    GIT_TAG v0.15.0
    GIT_SHALLOW FALSE
  )

  if(BUILD_TESTING)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.5.2
    )
  endif()

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
  # apc's version tracks the APC release it mirrors (v0.12.0 = APC v0.12.0).
  FetchContent_Declare(
    apc
    GIT_REPOSITORY https://github.com/pals-project/AtomicAndPhysicalConstantsCLib.git
    GIT_TAG v0.12.0
  )
  set(APC_BUILD_TESTS OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(rapidyaml)
  if(BUILD_TESTING)
    FetchContent_MakeAvailable(Catch2)
  endif()
  FetchContent_MakeAvailable(pcre2)
  FetchContent_MakeAvailable(apc)
  set(PALS_DEPENDENCIES ryml pcre2-8-static apc)
endif()

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
target_link_libraries(PALSParserCpp PRIVATE ${PALS_DEPENDENCIES})
# Dependencies installed alongside this library are discoverable after installation.
# Packaging tools may rewrite this for their own artifact layout.
if(APPLE)
  set_target_properties(PALSParserCpp PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(PALSParserCpp PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
# Public C header has no dependency on rapidyaml or the C++ implementation.
install(TARGETS PALSParserCpp
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT PALSParserCpp
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT PALSParserCpp
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT PALSParserCpp)
install(FILES src/PALSParserCpp.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT PALSParserCpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/PALSParserCpp COMPONENT PALSParserCpp)
endif()

if(PALS_BUILD_EXAMPLES)
  add_executable(example_rw examples/example_rw.cpp)
  target_link_libraries(example_rw PALSParserCpp ${PALS_DEPENDENCIES})

  add_executable(print_lattices examples/print_lattices.cpp)
  target_link_libraries(print_lattices PALSParserCpp ${PALS_DEPENDENCIES})
endif()

# Must come after PALSParserCpp is defined: tests/CMakeLists.txt links it.
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
