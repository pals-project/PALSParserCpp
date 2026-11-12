#include "test_helpers.h"

// ============================================================
// build_correspondence_map
// ============================================================

TEST_CASE("build_correspondence_map is empty for null handles", "[correspondence]") {
    struct correspondence_map m = build_correspondence_map(nullptr, nullptr, nullptr, nullptr);
    REQUIRE(m.count == 0);
    REQUIRE(m.links == nullptr);
    free_correspondence_map(m);  // must not crash
}

TEST_CASE("build_correspondence_map links the document roots", "[correspondence]") {
    struct lattices lat =
        parse_and_expand_PALS(lattice_file("ex.pals.yaml").c_str(), nullptr);
    struct correspondence_map m =
        build_correspondence_map(lat.original, lat.combined, lat.full_expanded,
                                 lat.adjunct);

    REQUIRE(m.count > 0);

    // adjunct is what still carries the document root, so that is the node
    // corresponding to the combined root. (The expanded root is synthesised to
    // hold the lattice entry and has no counterpart — see below.)
    YAMLNodeId left_root = get_root(lat.adjunct);

    bool found_root = false;
    for (size_t i = 0; i < m.count; i++) {
        if (m.links[i].adjunct == left_root) {
            found_root = true;
            REQUIRE(m.links[i].combined == get_root(lat.combined));
            // The original tree's first child is the top-level file's contents.
            REQUIRE(m.links[i].original ==
                    get_child_by_index(lat.original, get_root(lat.original), 0));
            REQUIRE(is_map(lat.combined, m.links[i].combined));
            REQUIRE(is_map(lat.original, m.links[i].original));
        }
        // A link names a node in exactly one of the two derived trees.
        REQUIRE((m.links[i].full_expanded == YAML_NULL_ID) !=
                (m.links[i].adjunct == YAML_NULL_ID));
    }
    REQUIRE(found_root);

    free_correspondence_map(m);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// A definition that expansion substituted into the lattice is a copy: the
// definition still stands in adjunct, so the same combined node reaches both
// trees.
TEST_CASE("build_correspondence_map ties the two trees through combined",
          "[correspondence]") {
    struct lattices lat =
        parse_and_expand_PALS(lattice_file("ex.pals.yaml").c_str(), "lat1");
    struct correspondence_map m =
        build_correspondence_map(lat.original, lat.combined, lat.full_expanded,
                                 lat.adjunct);

    // inj_line is defined at facility level and used by lat1's branches, so it
    // is expanded into lat1 while its definition stays in adjunct.
    std::map<YAMLNodeId, int> sides;  // combined id -> bitmask of trees reached
    for (size_t i = 0; i < m.count; i++) {
        if (m.links[i].combined == YAML_NULL_ID) continue;
        sides[m.links[i].combined] |=
            (m.links[i].full_expanded != YAML_NULL_ID) ? 1 : 2;
    }

    bool found_both = false;
    for (const auto& kv : sides)
        if (kv.second == 3) found_both = true;
    REQUIRE(found_both);

    free_correspondence_map(m);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
    free_lattice_problems(lat.problems);
}

TEST_CASE("build_correspondence_map connects a node across trees by value",
          "[correspondence]") {
    // A constant that lives outside the expanded lattice is not part of it, so
    // it lands in adjunct; the map must still connect it back to combined and
    // original.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - constants:\n"
        "        - a_const: 0.3 * r_electron\n"
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        length: 1.0\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - q1\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    struct correspondence_map m =
        build_correspondence_map(lat.original, lat.combined, lat.full_expanded,
                                 lat.adjunct);

    // Locate a_const in the adjunct tree: facility[0] -> constants[0] -> a_const.
    YAMLNodeId l_const = get_child_by_index(lat.adjunct, facility_of(lat.adjunct), 0);
    YAMLNodeId l_a_const =
        compact_def(lat.adjunct,
                         get_child_by_key(lat.adjunct, l_const, "constants"),
                         "a_const");
    REQUIRE(l_a_const != YAML_NULL_ID);
    // Expressions are evaluated across the whole document before it is split, so
    // this constant holds a number in adjunct too; the combined/original copies
    // (checked below) still carry the original expression text.
    {
        char* s = as_string(lat.adjunct, l_a_const);
        REQUIRE(s != nullptr);
        double got = std::strtod(s, nullptr);
        yaml_free_string(s);
        bool okc = false;
        double want = evaluate_pals_expression("0.3 * r_electron", &okc);
        REQUIRE(okc);
        REQUIRE(got == want);
    }

    // Find its link and follow it to the combined and original copies.
    bool found = false;
    for (size_t i = 0; i < m.count; i++) {
        if (m.links[i].adjunct != l_a_const) continue;
        found = true;
        REQUIRE(m.links[i].full_expanded == YAML_NULL_ID);
        REQUIRE(m.links[i].combined != YAML_NULL_ID);
        REQUIRE(m.links[i].original != YAML_NULL_ID);
        REQUIRE(val_eq(lat.combined, m.links[i].combined, "0.3 * r_electron"));
        REQUIRE(val_eq(lat.original, m.links[i].original, "0.3 * r_electron"));
    }
    REQUIRE(found);

    free_correspondence_map(m);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("build_correspondence_map maps one source to many expanded copies",
          "[correspondence]") {
    // A `repeat` unrolls one combined element into several expanded nodes; the
    // map must link all copies back to a single combined source.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - d1:\n"
        "        kind: Drift\n"
        "        length: 2.0\n"
        "    - cell:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - d1\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - cell:\n"
        "              repeat: 3\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    struct correspondence_map m =
        build_correspondence_map(lat.original, lat.combined, lat.full_expanded,
                                 lat.adjunct);

    // Unrolling `repeat: 3` over a one-element cell produces three `d1` entries
    // in the expanded line, all copied from the same combined source. They are
    // maps keyed `d1` carrying the substituted definition: a copy is expanded
    // like any other line entry. (This test used to look for keyless `d1`
    // scalars, which is what the copies were left as while `repeat` spliced
    // them without expanding them.)
    std::map<YAMLNodeId, int> combined_hits;
    for (size_t i = 0; i < m.count; i++) {
        // Skip the adjunct half of the map: those ids index a different tree.
        if (m.links[i].full_expanded == YAML_NULL_ID) continue;
        if (!key_eq(lat.full_expanded, m.links[i].full_expanded, "d1")) continue;
        REQUIRE(m.links[i].combined != YAML_NULL_ID);  // has a source
        combined_hits[m.links[i].combined]++;
    }
    int max_copies = 0;
    for (auto& kv : combined_hits) max_copies = std::max(max_copies, kv.second);
    REQUIRE(max_copies >= 3);  // one combined node -> three expanded copies

    free_correspondence_map(m);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}
