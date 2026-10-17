include(CTest)
add_executable(tests
  test_yaml_api.cpp
  test_expansion.cpp
  test_correspondence.cpp
  test_key_order.cpp
  test_matching.cpp
  test_parameters.cpp
  test_expressions.cpp
  test_multipass.cpp
  test_controllers.cpp
  test_sets.cpp
  test_bookkeeper.cpp
  test_expanded_minus_dependent.cpp
  test_check.cpp
  test_problems.cpp
  test_load.cpp
  test_floor.cpp
  test_placement.cpp
  # pals_floor.cpp carries hidden visibility in the shared library, so its
  # geometry symbols are compiled directly into the tests for test_floor.cpp.
  ../src/pals_floor.cpp
)

target_link_libraries(tests
  PRIVATE
  PALSParserCpp
  Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(tests)
target_compile_features(tests PRIVATE cxx_std_17)

# Checks the standard example lattices from pals-project/pals/examples against
# this implementation via the public C API.
# Not a ctest test: the standard_examples GitHub workflow runs it against a
# fresh checkout of the standard, pinned to the commit named in
# tests/standard_examples_git_reference.txt. To run it locally against a checkout of
# https://github.com/pals-project/pals, from the repository root:
#   cmake --build build --target validate_standard_examples
#   build/tests/validate_standard_examples \
#     --root <path-to-pals>/examples \
#     --manifest tests/standard_examples_manifest.txt
add_executable(validate_standard_examples validate_standard_examples.cpp)
target_link_libraries(validate_standard_examples PRIVATE yaml_c_wrapper)
target_compile_features(validate_standard_examples PRIVATE cxx_std_17)
