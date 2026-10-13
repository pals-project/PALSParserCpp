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
