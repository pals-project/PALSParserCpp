#include "test_helpers.h"

// ============================================================
// PARAMETER VALUES
// ============================================================

TEST_CASE("get_parameter_value returns a set numeric value", "[param]") {
    Expanded x(MATCH_YAML);
    struct param_value v = get_parameter_value(x.full(), "Q1>length");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(0.5));

    v = get_parameter_value(x.full(), "B1b>BendP.e1");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(0.3));

    // Before expansion the definition answers.
    YAMLTreeHandle t = parse_string(MATCH_YAML);
    v = get_parameter_value(t, "B1a>BendP.e1");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(0.1));
    delete_tree(t);
}

TEST_CASE("get_parameter_value returns the default for an unset parameter",
          "[param]") {
    Expanded x(MATCH_YAML);
    // Q1 exists but has no BendP.g; the default (0, for now) comes back.
    struct param_value v = get_parameter_value(x.full(), "Q1>BendP.g");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == 0.0);

    // A name that is not a real parameter is indistinguishable from an unset one
    // without a schema, so it also takes the default.
    v = get_parameter_value(x.full(), "Q1>not_a_param");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == 0.0);
}

TEST_CASE("get_parameter_value returns a non-numeric value as a string",
          "[param]") {
    YAMLTreeHandle t = parse_string(
        "PALS:\n"
        "  facility:\n"
        "    - fl:\n"
        "        kind: Foil\n"
        "        length: 2 * 0.5\n"
        "        ReferenceP:\n"
        "          species_ref: \"#3He\"\n");
    // A species name is not numeric: returned as a string.
    struct param_value v = get_parameter_value(t, "fl>ReferenceP.species_ref");
    REQUIRE(v.kind == PARAM_VALUE_STRING);
    REQUIRE(v.string != nullptr);
    REQUIRE(std::string(v.string) == "#3He");
    yaml_free_string(v.string);

    // An element parameter whose value is an expression: returned verbatim,
    // not evaluated.
    v = get_parameter_value(t, "fl>length");
    REQUIRE(v.kind == PARAM_VALUE_STRING);
    REQUIRE(std::string(v.string) == "2 * 0.5");
    yaml_free_string(v.string);
    delete_tree(t);
}

TEST_CASE("get_parameter_value returns missing when nothing is identified",
          "[param]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);

    // No such element.
    REQUIRE(get_parameter_value(t, "nosuch>length").kind == PARAM_VALUE_MISSING);
    // No parameter path — a bare element names no parameter.
    REQUIRE(get_parameter_value(t, "B1a").kind == PARAM_VALUE_MISSING);
    // The path resolves to a whole parameter group, not a single value.
    REQUIRE(get_parameter_value(t, "B1a>BendP").kind == PARAM_VALUE_MISSING);
    // Malformed pattern.
    REQUIRE(get_parameter_value(t, "(unclosed>length").kind ==
            PARAM_VALUE_MISSING);
    // Null args.
    REQUIRE(get_parameter_value(nullptr, "B1a>length").kind ==
            PARAM_VALUE_MISSING);
    REQUIRE(get_parameter_value(t, nullptr).kind == PARAM_VALUE_MISSING);
    delete_tree(t);
}

TEST_CASE("get_parameter_value reads a constant or variable by bare name",
          "[param]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);

    // Compact-form constant with a plain numeric value.
    struct param_value v = get_parameter_value(t, "a_two");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(5.0));

    // Its value is an expression, returned verbatim (not evaluated).
    v = get_parameter_value(t, "a_const");
    REQUIRE(v.kind == PARAM_VALUE_STRING);
    REQUIRE(std::string(v.string) == "0.3 * r_electron");
    yaml_free_string(v.string);

    // Full-form variable: the value lives under a `value:` child.
    v = get_parameter_value(t, "my_var");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(37.0));

    // A pattern matching two differently-valued constants is a conflict.
    REQUIRE(get_parameter_value(t, "a_.*").kind == PARAM_VALUE_MISSING);

    // A bare element name is not a value.
    REQUIRE(get_parameter_value(t, "Q1").kind == PARAM_VALUE_MISSING);
    delete_tree(t);
}

TEST_CASE("constants are found under the original tree's per-file wrapper",
          "[param][matching]") {
    // The `original` view nests the whole document one level down, under a map
    // keyed by filename. Both match_names and get_parameter_value must reach the
    // PALS node there, not only when it sits at the root.
    YAMLTreeHandle t = parse_string(
        "\"/path/to/lat.pals.yaml\":\n"
        "  PALS:\n"
        "    facility:\n"
        "      - constants:\n"
        "          - a_const: 0.3 * 5\n");

    struct name_matches m = match_names(t, "a_const");
    REQUIRE(m.count == 1);
    free_name_matches(m);

    // The value is an expression, returned verbatim (the raw view keeps it).
    struct param_value v = get_parameter_value(t, "a_const");
    REQUIRE(v.kind == PARAM_VALUE_STRING);
    REQUIRE(std::string(v.string) == "0.3 * 5");
    yaml_free_string(v.string);
    delete_tree(t);
}

TEST_CASE("get_lattice_parameter_value tries expanded, then adjunct",
          "[param]") {
    // Stand-ins for the two live trees of a parse_and_expand_PALS() result: the
    // expanded lattice carries element parameters; the adjunct facility carries
    // constants and variables.
    YAMLTreeHandle expanded = parse_string(
        "fodo:\n"
        "  kind: Lattice\n"
        "  branches:\n"
        "    - main:\n"
        "        line:\n"
        "          - Q1: {kind: Quadrupole, length: 0.5}\n");
    YAMLTreeHandle adjunct = parse_string(
        "PALS:\n"
        "  facility:\n"
        "    - constants:\n"
        "        - a_two: 5\n");

    // An element parameter is found in the expanded tree.
    struct param_value v =
        get_lattice_parameter_value(expanded, adjunct, "Q1>length");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(0.5));

    // A constant is not in expanded, so adjunct is consulted.
    v = get_lattice_parameter_value(expanded, adjunct, "a_two");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(5.0));

    // Found in neither -> missing.
    REQUIRE(get_lattice_parameter_value(expanded, adjunct, "nope>x").kind ==
            PARAM_VALUE_MISSING);

    // Either handle may be NULL.
    REQUIRE(get_lattice_parameter_value(nullptr, adjunct, "a_two").kind ==
            PARAM_VALUE_NUMBER);
    REQUIRE(get_lattice_parameter_value(expanded, nullptr, "Q1>length").kind ==
            PARAM_VALUE_NUMBER);
    REQUIRE(get_lattice_parameter_value(expanded, nullptr, "a_two").kind ==
            PARAM_VALUE_MISSING);

    delete_tree(expanded);
    delete_tree(adjunct);
}

TEST_CASE("get_parameter_value collapses agreeing matches, rejects conflicts",
          "[param]") {
    Expanded x(MATCH_YAML);

    // B1a and B1b have different lengths: a conflict leaves the parameter
    // unidentified.
    REQUIRE(get_parameter_value(x.full(), "B1.>length").kind ==
            PARAM_VALUE_MISSING);

    // The two copies of B1a agree.
    struct param_value v = get_parameter_value(x.full(), "B1a>length");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(1.2));

    // B1a and B1b are both Bends; their equal `kind` collapses to one string.
    v = get_parameter_value(x.full(), "B1.>kind");
    REQUIRE(v.kind == PARAM_VALUE_STRING);
    REQUIRE(std::string(v.string) == "Bend");
    yaml_free_string(v.string);
}

TEST_CASE("get_parameter_value reads a controller variable", "[param]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);
    struct param_value v = get_parameter_value(t, "ps1>cur");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(v.number == Catch::Approx(0.1));
    delete_tree(t);
}
