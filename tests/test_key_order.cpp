#include "test_helpers.h"

// ============================================================
// KEY ORDER
// ============================================================

// A lattice file is meant to be read by people, so a map's keys must come back
// in the order the author wrote them, never sorted. Order is preserved rather
// than imposed: the YAML backend stores map children in a sequence, and the
// expansion passes copy children in order. Nothing asserts that on its own,
// which is what these tests are for.

// The keys of `node`'s children, in order. Keyless children (sequence items)
// contribute an empty string.
static std::vector<std::string> keys_of(YAMLTreeHandle tree, YAMLNodeId node) {
    std::vector<std::string> keys;
    for (size_t i = 0; i < get_size(tree, node); i++) {
        char* k = get_node_key(tree, get_child_by_index(tree, node, i));
        keys.push_back(k ? k : "");
        yaml_free_string(k);
    }
    return keys;
}

TEST_CASE("Map keys keep their file order through a parse/emit round-trip",
          "[key_order]") {
    // This one goes through files on purpose: the round trip is what is being
    // tested. lattices/key_order.yaml is deliberately not alphabetical --
    // sorting would give alpha, bravo, mike, zulu.
    const std::vector<std::string> expected = {"zulu", "alpha", "mike", "bravo"};

    YAMLTreeHandle tree = parse_file(lattice_file("key_order.yaml").c_str());
    REQUIRE(keys_of(tree, get_root(tree)) == expected);

    // ... and again after emitting and reading the result back.
    const std::string out = out_path("key_order_out.yaml");
    REQUIRE(write_file(tree, out.c_str()));
    YAMLTreeHandle reloaded = parse_file(out.c_str());
    REQUIRE(keys_of(reloaded, get_root(reloaded)) == expected);

    delete_tree(tree);
    delete_tree(reloaded);
    remove_file(out);
}

TEST_CASE("add_scalar inserts at the requested position", "[key_order]") {
    YAMLTreeHandle tree = parse_string("zulu: 1\nalpha: 2");
    YAMLNodeId root = get_root(tree);
    add_scalar(tree, root, "omega", "3", YAML_END);  // append
    add_scalar(tree, root, "first", "0", 0);    // prepend

    const std::vector<std::string> expected = {"first", "zulu", "alpha", "omega"};
    REQUIRE(keys_of(tree, root) == expected);

    delete_tree(tree);
}

TEST_CASE("Expansion preserves the key order of the source file", "[key_order]") {
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - thingB:\n"
        "        kind: Sextupole\n"
        "        length: 2\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        multipass: true\n"
        "        length: 37.8\n"
        "        zero_point: thingC\n"
        "        line:\n"
        "          - thingZ:\n"
        "              inherit: thingB\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: lat1\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);

    // `main_line`'s keys are not in alphabetical order — sorting would move
    // `length` above `line` and `multipass` to the end — so the order below
    // can only come from the source file.
    const std::vector<std::string> expected = {"kind", "multipass", "length",
                                               "zero_point", "line"};

    // The combined tree splices includes; `main_line` is facility item 1.
    YAMLNodeId c_line = get_child_by_index(
        lat.combined,
        get_child_by_index(lat.combined, facility_of(lat.combined), 1), 0);
    REQUIRE(key_eq(lat.combined, c_line, "main_line"));
    REQUIRE(keys_of(lat.combined, c_line) == expected);

    // The expanded tree is rooted at the lattice entry itself, and the line is
    // inlined under its `branches`. Inlining it made it a branch, which drops
    // the `kind: BeamLine` it had as a definition; the surviving keys keep the
    // order they were written in.
    const std::vector<std::string> expected_branch = {"multipass", "length",
                                                      "zero_point", "line"};
    YAMLNodeId lat1 = get_child_by_index(lat.full_expanded, get_root(lat.full_expanded), 0);
    REQUIRE(key_eq(lat.full_expanded, lat1, "lat1"));
    YAMLNodeId branches = get_child_by_key(lat.full_expanded, lat1, "branches");
    YAMLNodeId e_line = get_child_by_index(
        lat.full_expanded, get_child_by_index(lat.full_expanded, branches, 0), 0);
    REQUIRE(key_eq(lat.full_expanded, e_line, "main_line"));
    REQUIRE(keys_of(lat.full_expanded, e_line) == expected_branch);

    // Merging `inherit: thingB` brings the parent's keys in ahead of the
    // child's own, rather than sorting the merged result. `main_line` is a
    // `multipass` root line, so its one element also picks up a trailing
    // `multipass_index`, appended after the inherited keys. Finally, the element
    // bookkeeper appends its computed output groups (`ReferenceP`, `FloorP`),
    // `s_position`, `element_index` and `ShiftedFloorP`, in that order, after
    // everything the file supplied.
    YAMLNodeId line_seq = get_child_by_key(lat.full_expanded, e_line, "line");
    YAMLNodeId thingZ = get_child_by_index(
        lat.full_expanded, get_child_by_index(lat.full_expanded, line_seq, 0), 0);
    REQUIRE(key_eq(lat.full_expanded, thingZ, "thingZ"));
    const std::vector<std::string> inherited = {
        "kind",           "length",     "inherit",    "multipass_index",
        "ReferenceP",     "FloorP",     "s_position", "element_index",
        "ShiftedFloorP"};
    REQUIRE(keys_of(lat.full_expanded, thingZ) == inherited);
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, thingZ, "multipass_index"),
                   "1"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}
