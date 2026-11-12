#include "test_helpers.h"

// ============================================================
// parse_and_expand_PALS (smoke test — requires the example lattice files)
// ============================================================

TEST_CASE("parse_and_expand_PALS returns four non-null handles", "[lattices]") {
    struct lattices lat =
        parse_and_expand_PALS(lattice_file("ex.pals.yaml").c_str(), nullptr);
    REQUIRE(lat.original != nullptr);
    REQUIRE(lat.combined != nullptr);
    REQUIRE(lat.full_expanded != nullptr);
    REQUIRE(lat.adjunct != nullptr);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// The expanded tree holds the root lattice and nothing else: no PALS/facility
// wrapper, and only the one entry.
TEST_CASE("expanded holds only the root lattice", "[lattices]") {
    struct lattices lat =
        parse_and_expand_PALS(lattice_file("ex.pals.yaml").c_str(), "lat1");
    YAMLNodeId root = get_root(lat.full_expanded);

    REQUIRE(is_map(lat.full_expanded, root));
    REQUIRE(get_size(lat.full_expanded, root) == 1);
    REQUIRE(get_child_by_key(lat.full_expanded, root, "PALS") == YAML_NULL_ID);

    YAMLNodeId entry = get_child_by_index(lat.full_expanded, root, 0);
    char* key = get_node_key(lat.full_expanded, entry);
    REQUIRE(std::string(key) == "lat1");
    yaml_free_string(key);
    REQUIRE(val_eq(lat.full_expanded, get_child_by_key(lat.full_expanded, entry, "kind"),
                   "Lattice"));

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
    free_lattice_problems(lat.problems);
}

// Everything else stays behind, under its PALS/facility scaffolding — including
// the lattice that was not expanded.
TEST_CASE("adjunct keeps the rest of the document", "[lattices]") {
    struct lattices lat =
        parse_and_expand_PALS(lattice_file("ex.pals.yaml").c_str(), "lat1");
    YAMLNodeId fac = facility_of(lat.adjunct);
    REQUIRE(fac != YAML_NULL_ID);

    bool saw_lat1 = false, saw_lat2 = false, saw_thingB = false;
    for (size_t i = 0; i < get_size(lat.adjunct, fac); i++) {
        YAMLNodeId item = get_child_by_index(lat.adjunct, fac, i);
        if (get_size(lat.adjunct, item) != 1) continue;
        char* key = get_node_key(lat.adjunct,
                                 get_child_by_index(lat.adjunct, item, 0));
        std::string k(key ? key : "");
        yaml_free_string(key);
        if (k == "lat1") saw_lat1 = true;
        if (k == "lat2") saw_lat2 = true;
        if (k == "thingB") saw_thingB = true;
    }

    REQUIRE_FALSE(saw_lat1);  // moved to expanded
    REQUIRE(saw_lat2);        // a non-root Lattice goes to adjunct like anything else
    REQUIRE(saw_thingB);      // element definitions stay put

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
    free_lattice_problems(lat.problems);
}

// handle_fork writes the raw node id of the fork's destination element. That id
// is assigned before the lattice is cut out into its own tree, so it must be
// translated to survive the renumbering.
TEST_CASE("destination_pointer resolves inside the expanded tree", "[lattices]") {
    struct lattices lat =
        parse_and_expand_PALS(lattice_file("ex.pals.yaml").c_str(), "lat1");

    // Find the destination_pointer scalar anywhere in the expanded tree.
    YAMLNodeId fp = YAML_NULL_ID;
    std::vector<YAMLNodeId> stack{get_root(lat.full_expanded)};
    while (!stack.empty()) {
        YAMLNodeId n = stack.back();
        stack.pop_back();
        char* key = get_node_key(lat.full_expanded, n);
        if (key && std::string(key) == "destination_pointer") fp = n;
        yaml_free_string(key);
        for (size_t i = 0; i < get_size(lat.full_expanded, n); i++)
            stack.push_back(get_child_by_index(lat.full_expanded, n, i));
    }
    REQUIRE(fp != YAML_NULL_ID);

    char* val = as_string(lat.full_expanded, fp);
    YAMLNodeId target = (YAMLNodeId)std::stoull(val);
    yaml_free_string(val);

    // It names the fork's destination_element in the branch expansion created.
    char* dest = as_string(lat.full_expanded, target);
    REQUIRE(std::string(dest) == "dump_begin");
    yaml_free_string(dest);

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
    free_lattice_problems(lat.problems);
}

TEST_CASE("Expansion drops `kind: BeamLine` from every branch", "[lattices]") {
    // The three routes a branch gets its contents by, in one file: `main` by
    // name substitution, `alt` by `inherit`, and `to_dump` built by a Fork out
    // of `dump_line`. Each copies a BeamLine definition in, and none of the
    // copies may keep the definition's kind.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        length: 0.5\n"
        "    - dump_begin:\n"
        "        kind: Marker\n"
        "    - sub:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - q1\n"
        "    - dump_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - dump_begin\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        periodic: false\n"
        "        line:\n"
        "          - sub\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump_line\n"
        "                destination_element: dump_begin\n"
        "                new_branch: to_dump\n"
        "    - main:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - q1\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main\n"
        "          - alt:\n"
        "              inherit: ring\n"
        "              periodic: true\n"
        "    - use: lat1\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    YAMLNodeId lat1 = get_child_by_index(lat.full_expanded, get_root(lat.full_expanded), 0);
    YAMLNodeId branches = get_child_by_key(lat.full_expanded, lat1, "branches");
    REQUIRE(get_size(lat.full_expanded, branches) == 3);

    // The lattice itself is not a branch and keeps its kind.
    REQUIRE(val_eq(lat.full_expanded, get_child_by_key(lat.full_expanded, lat1, "kind"),
                   "Lattice"));

    const char* names[] = {"main", "alt", "to_dump"};
    for (size_t i = 0; i < 3; i++) {
        YAMLNodeId branch = get_child_by_index(
            lat.full_expanded, get_child_by_index(lat.full_expanded, branches, i), 0);
        REQUIRE(key_eq(lat.full_expanded, branch, names[i]));
        REQUIRE(get_child_by_key(lat.full_expanded, branch, "kind") == YAML_NULL_ID);
        REQUIRE(get_child_by_key(lat.full_expanded, branch, "line") != YAML_NULL_ID);
    }

    // A branch keeps the components that are its own. `periodic: true` also
    // survives the merge of `ring`'s `periodic: false`, as the branch setting
    // overrides the root BeamLine's.
    YAMLNodeId alt = get_child_by_index(
        lat.full_expanded, get_child_by_index(lat.full_expanded, branches, 1), 0);
    REQUIRE(val_eq(lat.full_expanded, get_child_by_key(lat.full_expanded, alt, "inherit"),
                   "ring"));
    REQUIRE(val_eq(lat.full_expanded, get_child_by_key(lat.full_expanded, alt, "periodic"),
                   "true"));

    // `sub` sits inside a `line:`, so it is a sub-line: expansion splices its
    // contents into the enclosing line rather than leaving a nested BeamLine.
    // `alt` inherits `ring`, whose line is `[sub, f1]`; after flattening,
    // `sub`'s only element `q1` takes its place, so line[0] is `q1` itself.
    YAMLNodeId first = get_child_by_index(
        lat.full_expanded,
        get_child_by_index(lat.full_expanded,
                           get_child_by_key(lat.full_expanded, alt, "line"), 0),
        0);
    REQUIRE(key_eq(lat.full_expanded, first, "q1"));
    REQUIRE(val_eq(lat.full_expanded, get_child_by_key(lat.full_expanded, first, "kind"),
                   "Quadrupole"));

    // Expansion copies a definition rather than moving it, and the definition
    // left standing in adjunct is a BeamLine still.
    YAMLNodeId l_main = facility_param(lat.adjunct, "main");
    REQUIRE(l_main != YAML_NULL_ID);
    REQUIRE(val_eq(lat.adjunct, get_child_by_key(lat.adjunct, l_main, "kind"),
                   "BeamLine"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// A branch's `inherit` is optional and defaults to the branch's own name
// (lattice-construction.md, s:lattice.construct), so `- ln:` with nothing but a
// `periodic` under it is the branch `ln` built from the BeamLine `ln`.
TEST_CASE("A branch with no inherit takes its root BeamLine from its name",
          "[lattices]") {
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - begin:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: \"electron\"\n"
        "          E_tot_ref: 1.0e9\n"
        "    - q:\n"
        "        kind: Quadrupole\n"
        "        length: 1\n"
        "    - ln:\n"
        "        kind: BeamLine\n"
        "        periodic: true\n"
        "        line:\n"
        "          - begin\n"
        "          - q\n"
        "    - machine:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ln:\n"
        "              periodic: false\n"
        "    - use: \"machine\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    YAMLNodeId machine =
        get_child_by_index(lat.full_expanded, get_root(lat.full_expanded), 0);
    YAMLNodeId branch = get_child_by_index(
        lat.full_expanded,
        get_child_by_index(lat.full_expanded,
                           get_child_by_key(lat.full_expanded, machine, "branches"),
                           0),
        0);
    REQUIRE(key_eq(lat.full_expanded, branch, "ln"));

    // The root line's contents are here, not just the two keys the file wrote.
    YAMLNodeId line = get_child_by_key(lat.full_expanded, branch, "line");
    REQUIRE(line != YAML_NULL_ID);
    REQUIRE(get_size(lat.full_expanded, line) >= 2);
    REQUIRE(key_eq(lat.full_expanded,
                   get_child_by_index(
                       lat.full_expanded, get_child_by_index(lat.full_expanded, line, 1), 0),
                   "q"));

    // The branch's own `periodic` still overrides the root BeamLine's, so the
    // defaulted inherit is merged on the same terms as a written one.
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, branch, "periodic"), "false"));

    // Nothing about the branch is a problem.
    for (size_t i = 0; i < lat.problems.count; ++i)
        REQUIRE(std::string(lat.problems.items[i].message).find("branch 'ln'") ==
                std::string::npos);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// A written `inherit` names the root BeamLine outright, and the default must not
// displace it even when the branch's own name is also a defined BeamLine.
TEST_CASE("An explicit branch inherit wins over the branch name", "[lattices]") {
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - begin:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: \"electron\"\n"
        "          E_tot_ref: 1.0e9\n"
        "    - q_named:\n"
        "        kind: Quadrupole\n"
        "        length: 1\n"
        "    - s_wanted:\n"
        "        kind: Sextupole\n"
        "        length: 2\n"
        "    - alt:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin\n"
        "          - q_named\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin\n"
        "          - s_wanted\n"
        "    - machine:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - alt:\n"
        "              inherit: ring\n"
        "    - use: \"machine\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    YAMLNodeId machine =
        get_child_by_index(lat.full_expanded, get_root(lat.full_expanded), 0);
    YAMLNodeId branch = get_child_by_index(
        lat.full_expanded,
        get_child_by_index(lat.full_expanded,
                           get_child_by_key(lat.full_expanded, machine, "branches"),
                           0),
        0);
    YAMLNodeId line = get_child_by_key(lat.full_expanded, branch, "line");
    REQUIRE(line != YAML_NULL_ID);
    // `ring`'s element, not `alt`'s.
    REQUIRE(key_eq(lat.full_expanded,
                   get_child_by_index(
                       lat.full_expanded, get_child_by_index(lat.full_expanded, line, 1), 0),
                   "s_wanted"));
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, branch, "inherit"), "ring"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// A branch that comes out of expansion with no elements is reported in its own
// right, rather than being left for some later check to trip over.
TEST_CASE("An empty branch is reported", "[lattices][problems]") {
    auto problems_for = [](const char* yaml) {
        struct lattices lat = expand_PALS_string(yaml, nullptr);
        std::vector<std::string> msgs;
        for (size_t i = 0; i < lat.problems.count; ++i)
            msgs.emplace_back(lat.problems.items[i].message);
        free_lattice_problems(lat.problems);
        delete_tree(lat.original);
        delete_tree(lat.combined);
        delete_tree(lat.expanded);
        delete_tree(lat.full_expanded);
        delete_tree(lat.adjunct);
        return msgs;
    };
    auto has = [](const std::vector<std::string>& msgs, const char* needle) {
        for (const std::string& m : msgs)
            if (m.find(needle) != std::string::npos) return true;
        return false;
    };

    // No BeamLine named `ln` at all: the branch names its root line by its own
    // key, and that key resolves to nothing.
    SECTION("root BeamLine is not defined") {
        auto msgs = problems_for("PALS:\n"
                                 "  facility:\n"
                                 "    - machine:\n"
                                 "        kind: Lattice\n"
                                 "        branches:\n"
                                 "          - ln:\n"
                                 "              periodic: false\n"
                                 "    - use: \"machine\"\n");
        REQUIRE(has(msgs, "branch 'ln': no root BeamLine 'ln' is defined"));
    }

    // The root line is defined, and empty. The name resolves, so the message is
    // about the branch's contents rather than about a missing definition.
    SECTION("root BeamLine has an empty line") {
        auto msgs = problems_for("PALS:\n"
                                 "  facility:\n"
                                 "    - ln:\n"
                                 "        kind: BeamLine\n"
                                 "        line: []\n"
                                 "    - machine:\n"
                                 "        kind: Lattice\n"
                                 "        branches:\n"
                                 "          - ln\n"
                                 "    - use: \"machine\"\n");
        REQUIRE(has(msgs, "branch 'ln': expanded to no elements"));
        REQUIRE_FALSE(has(msgs, "no root BeamLine"));
    }
}

TEST_CASE("parse_and_expand_PALS reports expansion problems",
          "[expr][lattices][problems]") {
    // Every silent failure of expansion/evaluation is surfaced in the
    // `problems` list: dangling line references, undefined inherit/repeat
    // targets, and expressions that cannot be evaluated.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - constants:\n"
        "        - a_const: 0.3 * undefined_thing\n"
        "    - thingB:\n"
        "        kind: Sextupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn2L: 0.1\n"
        "    - DH1A:\n"
        "        kind: Bend\n"
        "        BendP:\n"
        "          edge2_int: 0.02 * thingB>MagneticMultipoleP.NotThere\n"
        "          e1: 3 * missing_const\n"
        "    - ghost_child:\n"
        "        kind: Bend\n"
        "        inherit: ghost_ancestor\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - DH1A\n"
        "          - ghost_child\n"
        "          - NoSuchElement\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    // Collect the messages so the assertions do not depend on their order.
    std::vector<std::string> msgs;
    for (size_t i = 0; i < lat.problems.count; ++i)
        msgs.emplace_back(lat.problems.items[i].message);

    auto has = [&](const std::string& needle) {
        for (const std::string& m : msgs)
            if (m.find(needle) != std::string::npos) return true;
        return false;
    };

    REQUIRE(has("reference to undefined element or line 'NoSuchElement'"));
    REQUIRE(has("inherit: 'ghost_ancestor' is not defined"));
    REQUIRE(has("could not evaluate expression for constants.a_const"));
    REQUIRE(has("could not evaluate expression for BendP.edge2_int"));
    REQUIRE(has("could not evaluate expression for BendP.e1"));

    // `main_line` opens on a Bend that declares no reference, so the branch has
    // no species or energy to propagate either.
    REQUIRE(has("branch 'main_line': first element 'DH1A' has no reference "
                "species or energy"));

    // Exactly those six: plain names (`kind: Bend`, the line references that
    // DO resolve) are not reported, and duplicate copies made by expansion
    // collapse to one message each.
    REQUIRE(lat.problems.count == 6);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("repeat with an unusable count keeps the entry",
          "[lattices][problems]") {
    // A repeat count that cannot be read is reported *and* left alone. It used
    // to be reported and then unrolled zero times, which removed the entry: the
    // lattice came back with an empty `line`, silently missing a beamline.
    auto counts_rejected = [](const char* count) {
        std::string doc = std::string(
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
            "              repeat: ") + count + "\n" +
            "    - lat1:\n"
            "        kind: Lattice\n"
            "        branches:\n"
            "          - main_line\n"
            "    - use: \"lat1\"\n";

        struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
        REQUIRE(lat.full_expanded != nullptr);

        bool reported = false;
        for (size_t i = 0; i < lat.problems.count; ++i)
            if (std::string(lat.problems.items[i].message)
                    .find("repeat: invalid count") != std::string::npos)
                reported = true;

        // The `line` under main_line must still hold the cell entry rather than
        // having been emptied.
        YAMLNodeId line = find_by_key(lat.full_expanded, "line");
        bool kept = line != YAML_NULL_ID && get_size(lat.full_expanded, line) > 0;

        free_lattice_problems(lat.problems);
        delete_tree(lat.original);
        delete_tree(lat.combined);
        delete_tree(lat.expanded);
        delete_tree(lat.full_expanded);
        delete_tree(lat.adjunct);
        return reported && kept;
    };

    REQUIRE(counts_rejected("bogus"));  // not a number at all
    REQUIRE(counts_rejected("3x"));     // stoi would stop early and return 3
    REQUIRE(counts_rejected("1.5"));    // a count is a whole number
    // A negative count is not an unusable one: it is the reflection spelling
    // (beamlines.md, s:repetition), covered by the reflection cases below.
}

TEST_CASE("repeat expands the copies it splices", "[lattices]") {
    // `repeat: n` used to splice the definition's entries and then step past
    // them, so the copies were never expanded: the branch came out a list of
    // bare element names rather than elements, and the lattice read as empty
    // with nothing reported. Writing the sub-line out by hand always worked, so
    // the two spellings of the same line disagreed.
    //
    // `inner` is nested inside the repeated `cell` to pin the recursive case:
    // the copies are expanded, so a sub-line among them flattens in its turn.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - d1:\n"
        "        kind: Drift\n"
        "        length: 2.0\n"
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        length: 0.4\n"
        "    - inner:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - q1\n"
        "    - cell:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - d1\n"
        "          - inner\n"
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
    REQUIRE(lat.full_expanded != nullptr);

    for (size_t i = 0; i < lat.problems.count; ++i)
        REQUIRE(std::string(lat.problems.items[i].message).find("repeat") ==
                std::string::npos);

    // Three copies of a two-element cell, flattened: d1 q1 d1 q1 d1 q1.
    YAMLNodeId line = find_by_key(lat.expanded, "line");
    REQUIRE(line != YAML_NULL_ID);
    REQUIRE(get_size(lat.expanded, line) == 6);

    // Each entry is the element itself rather than a reference to it: a
    // single-key map naming the element, carrying the definition it was
    // substituted with. A bare name would have no children to ask for a `kind`.
    for (size_t i = 0; i < 6; i++) {
        YAMLNodeId entry = get_child_by_index(lat.expanded, line, i);
        YAMLNodeId def = get_child_by_index(lat.expanded, entry, 0);
        bool even = (i % 2 == 0);
        REQUIRE(key_eq(lat.expanded, def, even ? "d1" : "q1"));
        REQUIRE(val_eq(lat.expanded, get_child_by_key(lat.expanded, def, "kind"),
                       even ? "Drift" : "Quadrupole"));
    }

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// The names of the branch entries of an expanded document, in order. The
// reflection cases below are entirely about order, so this is all they ask for.
static std::vector<std::string> line_names(YAMLTreeHandle tree) {
    std::vector<std::string> names;
    YAMLNodeId line = find_by_key(tree, "line");
    if (line == YAML_NULL_ID) return names;
    for (size_t i = 0; i < get_size(tree, line); i++) {
        YAMLNodeId entry = get_child_by_index(tree, line, i);
        YAMLNodeId def = get_child_by_index(tree, entry, 0);
        char* key = get_node_key(tree, def);
        names.push_back(key ? key : "");
        yaml_free_string(key);
    }
    return names;
}

TEST_CASE("a negative repeat count reflects the elements it repeats",
          "[lattices]") {
    // `repeat: -n` is the reflection spelling (beamlines.md, s:repetition): n
    // copies whose elements come out in reverse order. It used to be rejected
    // as an unusable count, which left the entry unexpanded and the branch a
    // bare beamline name -- an error on a lattice the standard allows.
    //
    // `inner` is nested inside the reflected `cell` to pin the point that what
    // is reversed is the *expanded* elements: the sub-line flattens first, so
    // the elements it contributes are reversed along with the rest, rather than
    // the sub-line staying forward as one lump.
    auto doc = [](const char* count) {
        return std::string(
                   "PALS:\n"
                   "  facility:\n"
                   "    - a:\n"
                   "        kind: Drift\n"
                   "        length: 1.0\n"
                   "    - b:\n"
                   "        kind: Drift\n"
                   "        length: 1.0\n"
                   "    - c:\n"
                   "        kind: Drift\n"
                   "        length: 1.0\n"
                   "    - inner:\n"
                   "        kind: BeamLine\n"
                   "        line:\n"
                   "          - b\n"
                   "          - c\n"
                   "    - cell:\n"
                   "        kind: BeamLine\n"
                   "        line:\n"
                   "          - a\n"
                   "          - inner\n"
                   "    - main_line:\n"
                   "        kind: BeamLine\n"
                   "        line:\n"
                   "          - cell:\n"
                   "              repeat: ") +
               count + "\n" +
               "    - lat1:\n"
               "        kind: Lattice\n"
               "        branches:\n"
               "          - main_line\n"
               "    - use: \"lat1\"\n";
    };

    auto expanded_names = [&](const char* count) {
        struct lattices lat = expand_PALS_string(doc(count).c_str(), nullptr);
        REQUIRE(lat.full_expanded != nullptr);
        for (size_t i = 0; i < lat.problems.count; ++i)
            REQUIRE(std::string(lat.problems.items[i].message).find("repeat") ==
                    std::string::npos);
        std::vector<std::string> names = line_names(lat.expanded);
        free_lattice_problems(lat.problems);
        delete_tree(lat.original);
        delete_tree(lat.combined);
        delete_tree(lat.expanded);
        delete_tree(lat.full_expanded);
        delete_tree(lat.adjunct);
        return names;
    };

    // `cell` expands to a b c, so a single reflection is c b a ...
    REQUIRE(expanded_names("-1") == std::vector<std::string>{"c", "b", "a"});
    // ... and each of n reflected copies is reversed in its turn, the whole run
    // reading as the reverse of the n unreflected copies.
    REQUIRE(expanded_names("-3") ==
            std::vector<std::string>{"c", "b", "a", "c", "b", "a", "c", "b",
                                     "a"});
    // The positive count is the same list forwards, and -0 is 0: nothing.
    REQUIRE(expanded_names("2") ==
            std::vector<std::string>{"a", "b", "c", "a", "b", "c"});
    REQUIRE(expanded_names("-0").empty());
}

TEST_CASE("a reflection nested inside a reflection is undone", "[lattices]") {
    // Reflection composes: a line reflecting a sub-line that itself reflects
    // one puts that innermost stretch back the way it was written. This only
    // works because the outer reflection reverses the run *after* the copies
    // are expanded -- by then the inner reflection has already turned its own
    // stretch around, and the outer pass reverses the result.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - a:\n"
        "        kind: Drift\n"
        "        length: 1.0\n"
        "    - b:\n"
        "        kind: Drift\n"
        "        length: 1.0\n"
        "    - c:\n"
        "        kind: Drift\n"
        "        length: 1.0\n"
        "    - inner:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - b\n"
        "          - c\n"
        "    - cell:\n"          // a, then inner reflected: a c b
        "        kind: BeamLine\n"
        "        line:\n"
        "          - a\n"
        "          - inner:\n"
        "              repeat: -1\n"
        "    - main_line:\n"     // cell reflected: b c a
        "        kind: BeamLine\n"
        "        line:\n"
        "          - cell:\n"
        "              repeat: -1\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);
    for (size_t i = 0; i < lat.problems.count; ++i)
        REQUIRE(std::string(lat.problems.items[i].message).find("repeat") ==
                std::string::npos);

    REQUIRE(line_names(lat.expanded) ==
            std::vector<std::string>{"b", "c", "a"});

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("parse_and_expand_PALS reports a missing lattice",
          "[lattices][problems]") {
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - thingB:\n"
        "        kind: Sextupole\n"
        "        length: 0.3\n";

    struct lattices lat = expand_PALS_string(doc, "not_here");
    REQUIRE(lat.problems.count == 1);
    REQUIRE(std::string(lat.problems.items[0].message) == "lattice 'not_here' not found");

    // With no lattice to expand, expanded is an empty map and the whole document
    // lands in adjunct — both handles are still valid.
    REQUIRE(lat.full_expanded != nullptr);
    REQUIRE(lat.adjunct != nullptr);
    REQUIRE(get_size(lat.full_expanded, get_root(lat.full_expanded)) == 0);
    REQUIRE(facility_of(lat.adjunct) != YAML_NULL_ID);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("a Fork with a ForkP sequence is reported as wrong-shape, not missing",
          "[lattices][problems]") {
    // A ForkP written as a sequence (a stray leading dash) *is* present, so the
    // old "missing ForkP" message was misleading. The diagnostic must say the
    // ForkP is the wrong shape rather than claim it is absent.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - dump_begin:\n"
        "        kind: Marker\n"
        "    - dump_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - dump_begin\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                - to_line: dump_line\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    bool wrong_shape = false;
    bool missing = false;
    for (size_t i = 0; i < lat.problems.count; ++i) {
        std::string msg = lat.problems.items[i].message;
        if (msg.find("f1") != std::string::npos) {
            if (msg.find("must be a map") != std::string::npos) wrong_shape = true;
            if (msg.find("missing ForkP") != std::string::npos) missing = true;
        }
    }
    REQUIRE(wrong_shape);
    REQUIRE_FALSE(missing);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("a Fork needs only to_line; destination_element and new_branch default",
          "[lattices][problems]") {
    // Per the schema only `to_line` is required: `destination_element` defaults
    // to the destination branch's beginning element and `new_branch` defaults to
    // the `to_line` name. A ForkP with just `to_line` must expand without any
    // "missing a required field" problem.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - begin:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "    - dump_begin:\n"
        "        kind: Marker\n"
        "    - dump_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - dump_begin\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump_line\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    for (size_t i = 0; i < lat.problems.count; ++i)
        REQUIRE(std::string(lat.problems.items[i].message).find("f1") ==
                std::string::npos);

    // The fork resolved to a target, so its ForkP carries a
    // destination_pointer and the defaulted branch (named after to_line)
    // exists. The pointer is a parameter of the group, not a key loose on the
    // element.
    YAMLNodeId forkp = find_by_key(lat.full_expanded, "ForkP");
    REQUIRE(get_child_by_key(lat.full_expanded, forkp, "destination_pointer") !=
            YAML_NULL_ID);

    // Expansion resolves ForkP.new_branch to the name of the branch it made,
    // which for the default is the to_line name.
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, forkp, "new_branch"),
                   "dump_line"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// Helper: the keyed element definition of a branch's first line element.
// expanded -> lat1(0) -> branches -> branch entry(idx) -> branch map(0) ->
// line -> line entry(pos) -> element def(0).
static YAMLNodeId element_of_branch(YAMLTreeHandle t, size_t branch_idx,
                                    size_t pos) {
    YAMLNodeId lat1 = get_child_by_index(t, get_root(t), 0);
    YAMLNodeId branches = get_child_by_key(t, lat1, "branches");
    YAMLNodeId branch = get_child_by_index(
        t, get_child_by_index(t, branches, branch_idx), 0);
    YAMLNodeId line = get_child_by_key(t, branch, "line");
    YAMLNodeId entry = get_child_by_index(t, line, pos);
    return get_child_by_index(t, entry, 0);
}

static YAMLNodeId first_element_of_branch(YAMLTreeHandle t, size_t branch_idx) {
    return element_of_branch(t, branch_idx, 0);
}

TEST_CASE("a Fork propagates reference and floor into its new branch",
          "[lattices]") {
    // propagate_reference is true by default, so the destination branch's
    // beginning element inherits the Fork element's reference species/energy and
    // floor placement (fork.md), even when the forked-to line has no BeginningEle
    // of its own.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - begin:\n"
        "        kind: BeginningEle\n"
        "        FloorP:\n"
        "          x: 1.5\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "    - m1:\n"
        "        kind: Marker\n"
        "    - ext_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - m1\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: ext_line\n"
        "                new_branch: extraction\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    // Branch 1 is `extraction`; its first (destination) element is `m1`.
    YAMLNodeId dest = first_element_of_branch(lat.full_expanded, 1);
    REQUIRE(key_eq(lat.full_expanded, dest, "m1"));

    YAMLNodeId refp = get_child_by_key(lat.full_expanded, dest, "ReferenceP");
    REQUIRE(refp != YAML_NULL_ID);
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, refp, "species_ref"),
                   "proton"));
    // Energy carried through the zero-length Fork unchanged, and completion
    // filled in the momentum from it.
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, refp, "E_tot_ref"), "1e+09"));
    REQUIRE(get_child_by_key(lat.full_expanded, refp, "pc_ref") != YAML_NULL_ID);

    // Floor placement of the source (x = 1.5) reaches the destination too.
    YAMLNodeId floorp = get_child_by_key(lat.full_expanded, dest, "FloorP");
    REQUIRE(floorp != YAML_NULL_ID);
    REQUIRE(val_eq(lat.full_expanded, get_child_by_key(lat.full_expanded, floorp, "x"),
                   "1.5"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("propagate_reference: false leaves the new branch's reference unset",
          "[lattices][problems]") {
    // With propagate_reference explicitly false, nothing is carried across the
    // Fork: the destination element keeps only its own (here empty) reference.
    // That leaves the whole branch with no reference to propagate, which is
    // reported -- the destination declares no ReferenceP of its own, so turning
    // propagation off is the same as never giving the branch one.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - begin:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "    - m1:\n"
        "        kind: Marker\n"
        "    - ext_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - m1\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: ext_line\n"
        "                new_branch: extraction\n"
        "                propagate_reference: false\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    YAMLNodeId dest = first_element_of_branch(lat.full_expanded, 1);
    REQUIRE(key_eq(lat.full_expanded, dest, "m1"));

    YAMLNodeId refp = get_child_by_key(lat.full_expanded, dest, "ReferenceP");
    // A ReferenceP is still written (time_ref), but no species/energy propagated.
    REQUIRE(get_child_by_key(lat.full_expanded, refp, "species_ref") == YAML_NULL_ID);
    REQUIRE(get_child_by_key(lat.full_expanded, refp, "E_tot_ref") == YAML_NULL_ID);

    // `ring` has its reference from `begin`; only `extraction` is reported.
    REQUIRE(lat.problems.count == 1);
    REQUIRE(std::string(lat.problems.items[0].message) ==
            "branch 'extraction': first element 'm1' has no reference species "
            "or energy, and none was propagated into the branch; the reference "
            "parameters cannot be computed");

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("a half-declared branch reference is reported for what it lacks",
          "[lattices][problems]") {
    // Species and energy are reported separately, because either alone leaves
    // the reference unusable: the species supplies the mass that converts
    // between energy and momentum, and without one of those two there is
    // nothing to convert. `pc_ref` counts as the energy half -- completion
    // derives `E_tot_ref` from it.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - no_energy:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "    - no_species:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          E_tot_ref: 1.0e9\n"
        "    - by_momentum:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          pc_ref: 1.0e9\n"
        "    - line_a:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - no_energy\n"
        "    - line_b:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - no_species\n"
        "    - line_c:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - by_momentum\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - line_a\n"
        "          - line_b\n"
        "          - line_c\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    std::vector<std::string> msgs;
    for (size_t i = 0; i < lat.problems.count; ++i)
        msgs.emplace_back(lat.problems.items[i].message);
    REQUIRE(msgs.size() == 2);

    REQUIRE(msgs[0].find("branch 'line_a': first element 'no_energy' has no "
                         "reference energy or momentum") == 0);
    REQUIRE(msgs[1].find("branch 'line_b': first element 'no_species' has no "
                         "reference species,") == 0);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("new_branch: null forks into an existing branch, creating none",
          "[lattices]") {
    // `new_branch: null` (fork.md, s:fork.params) points the Fork at an existing
    // branch instead of instantiating one. No branch is added, and because the
    // destination is not the beginning element of a *new* branch, nothing is
    // propagated: the existing branch keeps its own reference. (`other` is listed
    // first so it is already expanded when `ring`'s Fork resolves.)
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - begin:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "    - eb:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: electron\n"
        "          E_tot_ref: 5.0e8\n"
        "    - other:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - eb\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: other\n"
        "                new_branch: null\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - other\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    for (size_t i = 0; i < lat.problems.count; ++i)
        REQUIRE(std::string(lat.problems.items[i].message).find("f1") ==
                std::string::npos);

    // Exactly the two declared branches: the Fork added none.
    YAMLNodeId lat1 = get_child_by_index(lat.full_expanded, get_root(lat.full_expanded), 0);
    YAMLNodeId branches = get_child_by_key(lat.full_expanded, lat1, "branches");
    REQUIRE(get_size(lat.full_expanded, branches) == 2);

    // The Fork still connects: it carries a destination_pointer.
    REQUIRE(find_by_key(lat.full_expanded, "destination_pointer") != YAML_NULL_ID);

    // ForkP.new_branch stays `null` — no branch was made.
    YAMLNodeId forkp = find_by_key(lat.full_expanded, "ForkP");
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, forkp, "new_branch"),
                   "null"));

    // `other` (branch 0) keeps its own reference — the Fork's proton/1e9 was not
    // propagated into an existing branch.
    YAMLNodeId dest = first_element_of_branch(lat.full_expanded, 0);
    REQUIRE(key_eq(lat.full_expanded, dest, "eb"));
    YAMLNodeId refp = get_child_by_key(lat.full_expanded, dest, "ReferenceP");
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, refp, "species_ref"),
                   "electron"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("a destination of a kind that cannot be forked to is reported",
          "[lattices][problems]") {
    // Only a Marker, a BeginningEle, or another Fork may be forked to
    // (lattice-construction.md, s:fork): the destination has to share the Fork's
    // zero length and unit transfer map. `dump_line` begins with a Drift, so
    // both the defaulted destination and one named outright are rejected, and
    // neither Fork is linked.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - d1:\n"
        "        kind: Drift\n"
        "        length: 1\n"
        "    - dump_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - d1\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump_line\n"
        "          - f2:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump_line\n"
        "                new_branch: null\n"
        "                destination_element: d1\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "          - dump_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    int reported = 0;
    for (size_t i = 0; i < lat.problems.count; ++i) {
        std::string p(lat.problems.items[i].message);
        if (p.find("must be a Marker") != std::string::npos) {
            REQUIRE(p.find("'d1'") != std::string::npos);
            REQUIRE(p.find("'Drift'") != std::string::npos);
            reported++;
        }
    }
    REQUIRE(reported == 2);

    // Neither Fork resolved, so nothing was linked in either direction.
    REQUIRE(find_by_key(lat.full_expanded, "destination_pointer") == YAML_NULL_ID);
    REQUIRE(find_by_key(lat.full_expanded, "ForkFromP") == YAML_NULL_ID);
    REQUIRE(find_by_key(lat.full_expanded, "forked_to") == YAML_NULL_ID);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("a fork destination lists its incoming Forks in ForkFromP",
          "[lattices]") {
    // ForkFromP (forkfrom.md, s:fork.from.params) is the reverse link of ForkP:
    // the destination element carries one entry per Fork pointing at it, keyed
    // `{branch-name}>>{element-name}` with the Fork's 1-based index in its own
    // branch's line. Two Forks in `ring` aim at the same element, so both land
    // in the one group, in branch order.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - m0:\n"
        "        kind: Marker\n"
        "    - dump_begin:\n"
        "        kind: Marker\n"
        "    - dump_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - dump_begin\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - m0\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump_line\n"
        "          - m0\n"
        "          - f2:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump_line\n"
        "                new_branch: null\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    // f1 built the `dump_line` branch; f2 (new_branch: null) pointed into it.
    YAMLNodeId dest = first_element_of_branch(lat.full_expanded, 1);
    REQUIRE(key_eq(lat.full_expanded, dest, "dump_begin"));

    YAMLNodeId group = get_child_by_key(lat.full_expanded, dest, "ForkFromP");
    REQUIRE(group != YAML_NULL_ID);
    REQUIRE(get_size(lat.full_expanded, group) == 2);

    // f1 is the 2nd element of ring, f2 the 4th.
    YAMLNodeId e0 = get_child_by_index(
        lat.full_expanded, get_child_by_index(lat.full_expanded, group, 0), 0);
    REQUIRE(key_eq(lat.full_expanded, e0, "ring>>f1"));
    REQUIRE(val_eq(lat.full_expanded, e0, "2"));

    YAMLNodeId e1 = get_child_by_index(
        lat.full_expanded, get_child_by_index(lat.full_expanded, group, 1), 0);
    REQUIRE(key_eq(lat.full_expanded, e1, "ring>>f2"));
    REQUIRE(val_eq(lat.full_expanded, e1, "4"));

    // The group is built only for elements a Fork actually points at.
    REQUIRE(get_child_by_key(lat.full_expanded, first_element_of_branch(lat.full_expanded, 0),
                             "ForkFromP") == YAML_NULL_ID);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// Read `ForkP.forked_to` off an element of a branch line.
static std::string forked_to_of(YAMLTreeHandle t, size_t branch_idx,
                                size_t pos) {
    YAMLNodeId forkp = get_child_by_key(
        t, element_of_branch(t, branch_idx, pos), "ForkP");
    if (forkp == YAML_NULL_ID) return "";
    char* v = as_string(t, get_child_by_key(t, forkp, "forked_to"));
    std::string out(v ? v : "");
    yaml_free_string(v);
    return out;
}

TEST_CASE("a Fork names its destination in ForkP.forked_to", "[lattices]") {
    // `forked_to` (fork.md, s:fork.params) is an output parameter: the parser
    // writes the destination element as `{branch-name}>>{element-name}`. The
    // branch name is the one the expanded lattice ends up with, which is not in
    // general the `to_line` the input named -- all three settings of
    // `new_branch` are here, and only the SELF one has the two coincide.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - m0:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "    - dump_begin:\n"
        "        kind: Marker\n"
        "    - alt_begin:\n"
        "        kind: Marker\n"
        "    - dump_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - dump_begin\n"
        "    - alt_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - alt_begin\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - m0\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump_line\n"
        "                destination_element: dump_begin\n"
        "                new_branch: proton_dump\n"
        "          - f2:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: alt_line\n"
        "          - f3:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: proton_dump\n"
        "                new_branch: null\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);
    REQUIRE(lat.problems.count == 0);

    // f1 renamed the branch it built, so `forked_to` names `proton_dump` where
    // `to_line` said `dump_line`.
    REQUIRE(forked_to_of(lat.full_expanded, 0, 1) == "proton_dump>>dump_begin");

    // f2 took the default (SELF): the branch is named after the beam line, and
    // the destination defaults to that line's first element.
    REQUIRE(forked_to_of(lat.full_expanded, 0, 2) == "alt_line>>alt_begin");

    // f3 pointed into the branch f1 had already built, and names the same
    // destination as f1 does.
    REQUIRE(forked_to_of(lat.full_expanded, 0, 3) == "proton_dump>>dump_begin");

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("a Fork inside a forked-to branch resolves exactly once",
          "[lattices]") {
    // A chained fork: `ring` forks to `mid`, and `mid` itself forks to `end`.
    // handle_fork expands `mid` eagerly, the moment it has to look inside it for
    // the destination element, and appends it to `branches` -- which the
    // enclosing walk over `branches` is still iterating. Expanding it a second
    // time would re-run the Fork it holds, giving two `end` branches and two
    // destination_pointers on the one element, so the branch is expanded only once.
    //
    // `ring` is followed by a second listed branch, so the walk over `branches`
    // has somewhere to go after `ring` and reaches the entry `ring`'s Fork
    // appended behind it. A Fork in the *last* listed branch never showed the
    // duplication: the walk stops before the entry it added.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - m1:\n"
        "        kind: Marker\n"
        "    - m2:\n"
        "        kind: Marker\n"
        "    - end_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - m1\n"
        "    - mid_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - f2:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: end_line\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: mid_line\n"
        "    - spectator:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - m2\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "          - spectator\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    // Exactly four branches: the two listed, and one each from the two Forks.
    YAMLNodeId lat1 = get_child_by_index(lat.full_expanded, get_root(lat.full_expanded), 0);
    YAMLNodeId branches = get_child_by_key(lat.full_expanded, lat1, "branches");
    REQUIRE(get_size(lat.full_expanded, branches) == 4);

    // f2, the Fork in the branch that f1 built, ran once: its ForkP holds one
    // destination_pointer. A second run would append a second one to the same
    // group, since handle_fork reuses the ForkP it finds.
    YAMLNodeId f2 = first_element_of_branch(lat.full_expanded, 2);
    REQUIRE(key_eq(lat.full_expanded, f2, "f2"));
    YAMLNodeId forkp = get_child_by_key(lat.full_expanded, f2, "ForkP");
    int pointers = 0;
    for (size_t i = 0; i < get_size(lat.full_expanded, forkp); ++i) {
        char* k = get_node_key(lat.full_expanded,
                               get_child_by_index(lat.full_expanded, forkp, i));
        if (k && std::string(k) == "destination_pointer") pointers++;
        yaml_free_string(k);
    }
    REQUIRE(pointers == 1);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// Every node keyed `key` in the tree, in walk order.
static std::vector<std::string> all_values_for(YAMLTreeHandle t,
                                               const char* key) {
    std::vector<std::string> out;
    std::vector<YAMLNodeId> stack{get_root(t)};
    while (!stack.empty()) {
        YAMLNodeId n = stack.back();
        stack.pop_back();
        char* k = get_node_key(t, n);
        bool hit = k && std::string(k) == key;
        yaml_free_string(k);
        if (hit) {
            char* v = as_string(t, n);
            out.push_back(v ? std::string(v) : std::string());
            yaml_free_string(v);
        }
        for (size_t i = 0; i < get_size(t, n); i++)
            stack.push_back(get_child_by_index(t, n, i));
    }
    return out;
}

TEST_CASE("a destination_pointer survives expression substitution intact",
          "[lattices][problems]") {
    // A destination_pointer holds a node id, which is a number, so the expression pass
    // used to "evaluate" it and write the result back through format_double --
    // the shortest text that round-trips as a double. For most ids that is the
    // digits themselves, but an id of 110 comes back as `1.1e+02`, and every
    // reader parses the pointer with std::stoull, which stops at the `.` and
    // yields 1. That silently cost the fork its ForkFromP entry and its
    // reference propagation, and left remap_destination_pointers reporting the target
    // as outside the lattice. non_expr_keys() now skips the key.
    //
    // Only a minority of ids format that way -- a multiple of 10 from 100 up --
    // so the three unused `pad` Drifts below are there to shift the work-tree
    // ids until two of the five destinations land in that range. Without them
    // every pointer happens to come out in plain digits and the bug hides. The
    // assertions hold whatever the ids turn out to be; they simply stop
    // exercising this if an unrelated change shifts the ids again.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - begin1:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: \"electron\"\n"
        "          E_tot_ref: 1e7\n"
        "    - m:\n"
        "        kind: Marker\n"
        "    - dft:\n"
        "        kind: Drift\n"
        "        length: 2\n"
        "    - pad1:\n"
        "        kind: Drift\n"
        "        length: 1\n"
        "    - pad2:\n"
        "        kind: Drift\n"
        "        length: 1\n"
        "    - pad3:\n"
        "        kind: Drift\n"
        "        length: 1\n"
        "    - a_fork:\n"
        "        kind: Fork\n"
        "        ForkP:\n"
        "          to_line: a_line\n"
        "    - b_fork:\n"
        "        kind: Fork\n"
        "        ForkP:\n"
        "          to_line: b_line\n"
        "    - c_fork:\n"
        "        kind: Fork\n"
        "        ForkP:\n"
        "          to_line: c_line\n"
        "    - a_back_fork:\n"
        "        kind: Fork\n"
        "        ForkP:\n"
        "          to_line: a_line\n"
        "    - zero_line:\n"
        "        kind: BeamLine\n"
        "        line: [begin1, dft, a_fork]\n"
        "    - one_line:\n"
        "        kind: BeamLine\n"
        "        line: [begin1, dft, c_fork]\n"
        "    - a_line:\n"
        "        kind: BeamLine\n"
        "        line: [m, dft, b_fork]\n"
        "    - b_line:\n"
        "        kind: BeamLine\n"
        "        line: [m, dft]\n"
        "    - c_line:\n"
        "        kind: BeamLine\n"
        "        line: [m, dft, a_back_fork]\n"
        "    - root_lat:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - zero_line\n"
        "          - one_line\n"
        "    - use: \"root_lat\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);
    REQUIRE(lat.problems.count == 0);

    // Five Forks resolve: two chains of three (zero_line -> a_line -> b_line,
    // one_line -> c_line -> a_line -> b_line), sharing no branch instances.
    std::vector<std::string> ptrs = all_values_for(lat.full_expanded, "destination_pointer");
    REQUIRE(ptrs.size() == 5);
    for (const std::string& p : ptrs) {
        INFO("destination_pointer: " << p);
        REQUIRE(!p.empty());
        REQUIRE(p.find_first_not_of("0123456789") == std::string::npos);
    }

    // Each of those Forks reaches its destination, so each destination carries
    // the reverse link. A misparsed pointer drops the entry without a word.
    REQUIRE(all_values_for(lat.full_expanded, "ForkFromP").size() == 5);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("new_branch: null with a to_line that is not a branch is reported",
          "[lattices][problems]") {
    // `null` requires `to_line` to name an existing branch. A bare BeamLine
    // definition (not listed among the lattice's branches) does not qualify, and
    // the mismatch is surfaced rather than silently creating a branch.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - m1:\n"
        "        kind: Marker\n"
        "    - ext_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - m1\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: ext_line\n"
        "                new_branch: null\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - ring\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    bool reported = false;
    for (size_t i = 0; i < lat.problems.count; ++i)
        if (std::string(lat.problems.items[i].message).find("is not an existing branch") !=
            std::string::npos)
            reported = true;
    REQUIRE(reported);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("a document held in a string expands like one read from a file",
          "[lattices]") {
    // expand_PALS_string is parse_and_expand_PALS with the document in hand
    // rather than on disk. The two must agree, so one lattice is expanded both
    // ways and the finished trees compared. It is a document that names no other
    // file, which is the case where the two are meant to be interchangeable.
    const char* name = "load_basic/layout.pals.yaml";
    const std::string text = read_lattice(name);
    REQUIRE_FALSE(text.empty());

    struct lattices from_file =
        parse_and_expand_PALS(lattice_file(name).c_str(), nullptr);
    struct lattices from_string = expand_PALS_string(text.c_str(), nullptr);
    REQUIRE(from_string.full_expanded != nullptr);

    char* a = tree_to_string(from_file.full_expanded);
    char* b = tree_to_string(from_string.full_expanded);
    REQUIRE(std::string(a) == std::string(b));
    yaml_free_string(a);
    yaml_free_string(b);

    // The one difference is what `original` keys the document by: a path in the
    // first case, `<string>` in the second.
    REQUIRE(key_eq(from_string.original,
                   get_child_by_index(from_string.original,
                                      get_root(from_string.original), 0),
                   "<string>"));

    for (struct lattices* lat : {&from_file, &from_string}) {
        free_lattice_problems(lat->problems);
        delete_tree(lat->original);
        delete_tree(lat->combined);
        delete_tree(lat->expanded);
        delete_tree(lat->full_expanded);
        delete_tree(lat->adjunct);
    }
}

TEST_CASE("a malformed document held in a string is a fatal parse problem",
          "[lattices][problems]") {
    // The fatal path reports the string as `<string>`, there being no path to
    // name, and pinpoints the line the way it does for a file.
    struct lattices lat = expand_PALS_string(
        "PALS:\n"
        "  facility:\n"
        "    - cav\n"
        "        kind: RFCavity\n",
        nullptr);

    REQUIRE(lat.original == nullptr);
    REQUIRE(lat.problems.count == 1);
    std::string msg = lat.problems.items[0].message;
    REQUIRE(msg.find("<string>") != std::string::npos);
    REQUIRE(msg.find("line") != std::string::npos);

    free_lattice_problems(lat.problems);
}

TEST_CASE("a malformed top-level file is a fatal parse problem, not a crash",
          "[lattices][problems]") {
    // lattices/malformed.pals.yaml has a sequence item missing its ':' (`- cav`
    // followed by an indented mapping), a YAML syntax error. ryml would abort
    // the process; instead parse_and_expand_PALS must return NULL handles and
    // one problem that names the file and pinpoints the line. This one goes
    // through a file rather than a string so that the path in the message is
    // the one the caller passed in.
    const std::string path = lattice_file("malformed.pals.yaml");
    struct lattices lat = parse_and_expand_PALS(path.c_str(), nullptr);

    // No usable tree: every handle is NULL.
    REQUIRE(lat.original == nullptr);
    REQUIRE(lat.combined == nullptr);
    REQUIRE(lat.full_expanded == nullptr);
    REQUIRE(lat.adjunct == nullptr);

    // A single problem, naming the file and the offending line.
    REQUIRE(lat.problems.count == 1);
    std::string msg = lat.problems.items[0].message;
    REQUIRE(msg.find(path) != std::string::npos);
    REQUIRE(msg.find("line") != std::string::npos);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);  // all NULL — delete_tree(NULL) is a safe no-op
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}
