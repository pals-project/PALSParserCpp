#include "test_helpers.h"

// ============================================================
// LIBRARY VERSION
// ============================================================

TEST_CASE("pals_parser_cpp_version is the CMake project version", "[version]") {
    const char* v = pals_parser_cpp_version();
    REQUIRE(v != nullptr);
    REQUIRE(std::string(v) == PALS_PARSER_CPP_EXPECTED_VERSION);
}

TEST_CASE("pals_parser_cpp_version is major.minor.patch", "[version]") {
    // What a binding parses: three non-negative integers and nothing else.
    const std::string v = pals_parser_cpp_version();
    size_t dots = 0;
    bool digit_before = false;
    for (char c : v) {
        if (c == '.') {
            REQUIRE(digit_before);
            ++dots;
            digit_before = false;
        } else {
            REQUIRE(c >= '0');
            REQUIRE(c <= '9');
            digit_before = true;
        }
    }
    REQUIRE(digit_before);
    REQUIRE(dots == 2);
}
