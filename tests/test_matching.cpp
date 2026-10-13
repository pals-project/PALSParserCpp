#include "test_helpers.h"

// ============================================================
// NAME MATCHING
// ============================================================

namespace {

// The element_index of each node in `m`, in order.
std::vector<int> indices(YAMLTreeHandle t, const struct name_matches& m) {
    std::vector<int> out;
    for (size_t i = 0; i < m.count; ++i)
        out.push_back(static_cast<int>(
            num_val(t, get_child_by_key(t, m.nodes[i], "element_index"))));
    return out;
}

std::vector<int> match_indices(YAMLTreeHandle t, const char* spec) {
    struct name_matches m = match_names(t, spec);
    std::vector<int> out = indices(t, m);
    free_name_matches(m);
    return out;
}

size_t match_count(YAMLTreeHandle t, const char* spec) {
    struct name_matches m = match_names(t, spec);
    size_t n = m.count;
    free_name_matches(m);
    return n;
}

}  // namespace

TEST_CASE("match_names handles null args", "[matching]") {
    struct name_matches m = match_names(nullptr, "a_const");
    REQUIRE(m.count == 0);
    REQUIRE(m.nodes == nullptr);
    free_name_matches(m);

    YAMLTreeHandle t = parse_string(MATCH_YAML);
    m = match_names(t, nullptr);
    REQUIRE(m.count == 0);
    free_name_matches(m);
    delete_tree(t);
}

TEST_CASE("a bare name matches a single constant", "[matching]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);
    struct name_matches m = match_names(t, "a_const");
    REQUIRE(m.count == 1);
    REQUIRE(key_eq(t, m.nodes[0], "a_const"));
    REQUIRE(val_eq(t, m.nodes[0], "0.3 * r_electron"));
    free_name_matches(m);
    delete_tree(t);
}

TEST_CASE("a constant/variable pattern matches by name across forms",
          "[matching]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);
    // a_.* matches the two compact constants but not the full-form variable.
    struct name_matches m = match_names(t, "a_.*");
    REQUIRE(m.count == 2);
    REQUIRE(key_eq(t, m.nodes[0], "a_const"));
    REQUIRE(key_eq(t, m.nodes[1], "a_two"));
    free_name_matches(m);

    // A full-form variable is matched too; the returned node is its named node.
    m = match_names(t, "my_var");
    REQUIRE(m.count == 1);
    REQUIRE(key_eq(t, m.nodes[0], "my_var"));
    REQUIRE(is_map(t, m.nodes[0]));  // full form: kind/value live underneath
    free_name_matches(m);
    delete_tree(t);
}

TEST_CASE("compact constants written as a plain map are matched too",
          "[matching]") {
    // The compact form may be a YAML map (name: value) rather than a sequence
    // of single-key maps; both must be matched.
    YAMLTreeHandle t = parse_string(
        "PALS:\n"
        "  facility:\n"
        "    - constants:\n"
        "        a_const: 0.3 * r_electron\n"
        "        b_const: 0.45\n"
        "    - variables:\n"
        "        a_var: 5\n");
    struct name_matches m = match_names(t, ".*_const");
    REQUIRE(m.count == 2);
    REQUIRE(key_eq(t, m.nodes[0], "a_const"));
    REQUIRE(key_eq(t, m.nodes[1], "b_const"));
    free_name_matches(m);

    m = match_names(t, "a_var");
    REQUIRE(m.count == 1);
    REQUIRE(val_eq(t, m.nodes[0], "5"));
    free_name_matches(m);
    delete_tree(t);
}

TEST_CASE("the name pattern is a whole-name (anchored) match", "[matching]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);
    // "a" alone must not match "a_const" — the whole name has to match.
    REQUIRE(match_count(t, "a") == 0);
    delete_tree(t);
}

// ---- Before expansion: a tree holding the PALS node ----

TEST_CASE("before expansion an unqualified name reaches the facility "
          "definitions only",
          "[matching]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);

    // Q1 is defined once under facility; the `- Q1` in main is a reference.
    struct name_matches m = match_names(t, "Q1>length");
    REQUIRE(m.count == 1);
    REQUIRE(val_eq(t, m.nodes[0], "0.5"));
    free_name_matches(m);

    // S1 is written inside the line of `sub`, so no unqualified name reaches it.
    REQUIRE(match_count(t, "S1>length") == 0);

    // Beamlines, lattices, controllers and constants are not elements.
    REQUIRE(match_count(t, ".*>length") == 3);  // B1a, B1b, Q1
    delete_tree(t);
}

TEST_CASE("before expansion a beamline qualifier reaches its line, sub-lines "
          "included",
          "[matching]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);

    struct name_matches m = match_names(t, "sub>>S1>length");
    REQUIRE(m.count == 1);
    REQUIRE(val_eq(t, m.nodes[0], "0.2"));
    free_name_matches(m);

    // `main` uses `sub`, so S1 is an element of main too.
    m = match_names(t, "main>>S1>length");
    REQUIRE(m.count == 1);
    REQUIRE(val_eq(t, m.nodes[0], "0.2"));
    free_name_matches(m);

    // A bare reference in a line is an element of that line: it holds no
    // parameters yet, but it is what the qualified name selects.
    m = match_names(t, "main>>Q1");
    REQUIRE(m.count == 1);
    REQUIRE(val_eq(t, m.nodes[0], "Q1"));
    free_name_matches(m);

    // A lattice qualifier reaches nothing: no lattice has been built.
    REQUIRE(match_count(t, "lat1>>>Q1") == 0);
    delete_tree(t);
}

TEST_CASE("controllers and their variables are matched by name",
          "[matching]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);

    struct name_matches m = match_names(t, "Controller::ps.*");
    REQUIRE(m.count == 1);
    REQUIRE(key_eq(t, m.nodes[0], "ps1"));
    free_name_matches(m);

    m = match_names(t, "ps1>cur");
    REQUIRE(m.count == 1);
    REQUIRE(val_eq(t, m.nodes[0], "0.1"));
    free_name_matches(m);

    // Only a controller answers to the Controller kind.
    REQUIRE(match_count(t, "Controller::Q1") == 0);
    delete_tree(t);
}

// ---- After expansion: the expanded lattice ----

TEST_CASE("in an expanded lattice a name matches every copy of an element",
          "[matching]") {
    Expanded x(MATCH_YAML);
    YAMLTreeHandle t = x.full();

    REQUIRE(match_indices(t, "B1a") == std::vector<int>{2, 6});
    REQUIRE(match_indices(t, "S1") == std::vector<int>{5});

    struct name_matches m = match_names(t, "B1.*>BendP.e1");
    REQUIRE(m.count == 3);
    REQUIRE(val_eq(t, m.nodes[0], "0.1"));
    REQUIRE(val_eq(t, m.nodes[1], "0.3"));
    REQUIRE(val_eq(t, m.nodes[2], "0.1"));
    free_name_matches(m);

    // An ungrouped parameter resolves directly on the element.
    m = match_names(t, "Q1>length");
    REQUIRE(m.count == 1);
    REQUIRE(val_eq(t, m.nodes[0], "0.5"));
    free_name_matches(m);

    // Dropping the parameter matches the group; a missing one matches nothing.
    m = match_names(t, "B1b>BendP");
    REQUIRE(m.count == 1);
    REQUIRE(is_map(t, m.nodes[0]));
    free_name_matches(m);
    REQUIRE(match_count(t, "B1a>BendP.nope") == 0);
}

TEST_CASE("kind, branch and lattice qualifiers filter the match",
          "[matching]") {
    Expanded x(MATCH_YAML);
    YAMLTreeHandle t = x.full();

    REQUIRE(match_indices(t, "Quadrupole::.*") == std::vector<int>{4});
    REQUIRE(match_indices(t, "Bend::.*") == std::vector<int>{2, 3, 6});
    REQUIRE(match_count(t, "Sextupole::B1a") == 0);

    REQUIRE(match_indices(t, "main>>S1") == std::vector<int>{5});
    REQUIRE(match_count(t, "nobranch>>S1") == 0);

    REQUIRE(match_indices(t, "lat1>>>Q1") == std::vector<int>{4});
    REQUIRE(match_count(t, "lat2>>>Q1") == 0);
    REQUIRE(match_indices(t, "lat1>>>main>>Q1") == std::vector<int>{4});
}

TEST_CASE("`#N` keeps the N-th match and an index names a position",
          "[matching]") {
    Expanded x(MATCH_YAML);
    YAMLTreeHandle t = x.full();

    REQUIRE(match_indices(t, "B1a#2") == std::vector<int>{6});
    REQUIRE(match_indices(t, "Bend::.*#3") == std::vector<int>{6});
    REQUIRE(match_count(t, "B1a#3") == 0);

    REQUIRE(match_indices(t, "main>>4") == std::vector<int>{4});
    REQUIRE(match_indices(t, "4") == std::vector<int>{4});
}

TEST_CASE("a range runs to the next end element, wrapping round the line",
          "[matching]") {
    Expanded x(MATCH_YAML);
    YAMLTreeHandle t = x.full();

    REQUIRE(match_indices(t, "B1b:S1") == std::vector<int>{3, 4, 5});
    // S1 comes after B1b, so this one wraps: S1 to the end, then the start to
    // B1b.
    REQUIRE(match_indices(t, "S1:B1b") == std::vector<int>{5, 6, 7, 1, 2, 3});

    // The parameter path applies to the whole range.
    struct name_matches m = match_names(t, "Q1:S1>length");
    REQUIRE(m.count == 2);
    REQUIRE(val_eq(t, m.nodes[0], "0.5"));
    REQUIRE(val_eq(t, m.nodes[1], "0.2"));
    free_name_matches(m);
}

TEST_CASE("unions and intersections combine element sets", "[matching]") {
    Expanded x(MATCH_YAML);
    YAMLTreeHandle t = x.full();

    REQUIRE(match_indices(t, "Q1, S1") == std::vector<int>{4, 5});
    REQUIRE(match_indices(t, "Bend::.* & B1b:S1") == std::vector<int>{3});
    // `&` binds tighter than `,`: B1a, then the Quadrupoles among Q1.
    REQUIRE(match_indices(t, "B1a, Q1 & Quadrupole::.*") ==
            std::vector<int>{2, 6, 4});
}

TEST_CASE("operators inside a bracket group belong to the pattern",
          "[matching]") {
    Expanded x(MATCH_YAML);
    YAMLTreeHandle t = x.full();
    // The comma in {1,2} is part of the regex, not a union.
    REQUIRE(match_indices(t, "B\\d{1,2}a") == std::vector<int>{2, 6});
    REQUIRE(match_indices(t, "(B1a|Q1)") == std::vector<int>{2, 4, 6});
}

TEST_CASE("a MetaP component selects elements by its value", "[matching]") {
    Expanded x(MATCH_YAML);
    YAMLTreeHandle t = x.full();
    REQUIRE(match_indices(t, "alias::black") == std::vector<int>{2, 4, 6});
    REQUIRE(match_indices(t, "alias::bl.*") == std::vector<int>{2, 4, 6});
    REQUIRE(match_count(t, "alias::white") == 0);
}

TEST_CASE("no matches yields an empty result", "[matching]") {
    Expanded x(MATCH_YAML);
    struct name_matches m = match_names(x.full(), "nosuch>foo");
    REQUIRE(m.count == 0);
    REQUIRE(m.nodes == nullptr);
    free_name_matches(m);
}

TEST_CASE("a malformed pattern yields an empty result, not a crash",
          "[matching]") {
    YAMLTreeHandle t = parse_string(MATCH_YAML);
    REQUIRE(match_count(t, "(unclosed") == 0);
    REQUIRE(match_count(t, "Q1#0") == 0);
    REQUIRE(match_count(t, "Q1>length>x") == 0);
    delete_tree(t);
}
