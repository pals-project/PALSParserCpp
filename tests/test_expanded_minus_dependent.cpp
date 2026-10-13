#include "test_helpers.h"

// ============================================================
// THE `expanded` TREE: full_expanded MINUS THE COMPUTED PARAMETERS
// ============================================================
//
// parse_and_expand_PALS returns the expanded lattice twice. `full_expanded` is
// what the bookkeeper leaves behind, every dependent parameter computed;
// `expanded` is the same lattice pruned back to the author's inputs. These tests
// pin the boundary between the two — which is decided by what the lattice held
// at the moment before run_element_bookkeeper first ran, plus whatever a
// `post_expansion` `set` wrote afterwards.

namespace {

// A branch exercising each way a parameter can arrive:
//   begin  — an authored ReferenceP, which the bookkeeper then completes
//   q1     — one authored member of a multipole family, the rest derived
//   rf1    — an authored RFP gradient, plus defaults the bookkeeper materializes
//   mark   — an element with nothing but its kind
const char* SPLIT_YAML =
    "PALS:\n"
    "  facility:\n"
    "    - lat1:\n"
    "        kind: Lattice\n"
    "        branches:\n"
    "          - main:\n"
    "              kind: BeamLine\n"
    "              line:\n"
    "                - begin:\n"
    "                    kind: BeginningEle\n"
    "                    ReferenceP:\n"
    "                      species_ref: \"electron\"\n"
    "                      pc_ref: 1.0e9\n"
    "                - q1:\n"
    "                    kind: Quadrupole\n"
    "                    length: 0.5\n"
    "                    MagneticMultipoleP:\n"
    "                      Kn1: 0.3\n"
    "                - rf1:\n"
    "                    kind: RFCavity\n"
    "                    length: 1.0\n"
    "                    RFP:\n"
    "                      gradient: 1.0e6\n"
    "                - mark:\n"
    "                    kind: Marker\n"
    "    - use: lat1\n";

// The same lattice with a `set` under `post_expansion`, so the set lands on the
// expanded copy — after the first bookkeeper pass has
// already run. `Kn2L` is written; `Kn2` is what the second pass derives from it.
const char* POST_SET_YAML =
    "PALS:\n"
    "  facility:\n"
    "    - lat1:\n"
    "        kind: Lattice\n"
    "        branches:\n"
    "          - main:\n"
    "              kind: BeamLine\n"
    "              line:\n"
    "                - begin:\n"
    "                    kind: BeginningEle\n"
    "                    ReferenceP:\n"
    "                      species_ref: \"electron\"\n"
    "                      pc_ref: 1.0e9\n"
    "                - q1:\n"
    "                    kind: Quadrupole\n"
    "                    length: 0.5\n"
    "                    MagneticMultipoleP:\n"
    "                      Kn1: 0.3\n"
    "    - use: lat1\n"
    "  post_expansion:\n"
    "    - set:\n"
    "        parameter: \"q1>MagneticMultipoleP.Kn2L\"\n"
    "        value: 0.75\n";

// A fork whose destination resolves, so link_fork_connections has somewhere to
// write: it names the destination on the Fork (`forked_to`) and hangs a
// `ForkFromP` back-reference on the destination element. Both run after the
// bookkeeper. `propagate_reference` is deliberately left unwritten here so it
// arrives as a materialized default.
const char* FORK_YAML =
    "PALS:\n"
    "  facility:\n"
    "    - dump_begin:\n"
    "        kind: BeginningEle\n"
    "    - amarker:\n"
    "        kind: Marker\n"
    "    - generic_dump:\n"
    "        kind: BeamLine\n"
    "        line:\n"
    "          - dump_begin\n"
    "          - amarker\n"
    "    - lat1:\n"
    "        kind: Lattice\n"
    "        branches:\n"
    "          - main:\n"
    "              kind: BeamLine\n"
    "              line:\n"
    "                - begin:\n"
    "                    kind: BeginningEle\n"
    "                    ReferenceP:\n"
    "                      species_ref: \"electron\"\n"
    "                      pc_ref: 1.0e9\n"
    "                - to_dump:\n"
    "                    kind: Fork\n"
    "                    ForkP:\n"
    "                      to_line: generic_dump\n"
    "                      destination_element: dump_begin\n"
    "                      new_branch: this_dump\n"
    "    - use: lat1\n";

// A controller driving a parameter the author already wrote, plus a
// `post_expansion` `set`. Between them these are every way a
// value can be rewritten after it was first written: `Kn1L` is authored as 0.33,
// driven to 0.8 by the ABSOLUTE controller, and `length` is then overwritten by
// the post-expansion set.
const char* REWRITTEN_YAML =
    "PALS:\n"
    "  facility:\n"
    "    - ps27:\n"
    "        kind: Controller\n"
    "        controls:\n"
    "          - parameter: Qa.*>MagneticMultipoleP.Kn1L\n"
    "            expression: 0.8\n"
    "    - lat1:\n"
    "        kind: Lattice\n"
    "        branches:\n"
    "          - main:\n"
    "              kind: BeamLine\n"
    "              line:\n"
    "                - begin:\n"
    "                    kind: BeginningEle\n"
    "                    ReferenceP:\n"
    "                      species_ref: \"electron\"\n"
    "                      E_tot_ref: 1.0e9\n"
    "                - Qa1:\n"
    "                    kind: Quadrupole\n"
    "                    length: 0.5\n"
    "                    MagneticMultipoleP:\n"
    "                      Kn1L: 0.33\n"
    "    - use: lat1\n"
    "  post_expansion:\n"
    "    - set:\n"
    "        parameter: \"Qa1>length\"\n"
    "        value: 0.75\n";

void free_all(struct lattices& lat) {
    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// True when `ele` in `t` carries `group`, and `group` carries `param`. A null
// `group` looks the parameter up directly on the element.
bool has_param(YAMLTreeHandle t, const char* ele, const char* group,
               const char* param) {
    YAMLNodeId e = find_by_key(t, ele);
    if (e == YAML_NULL_ID) return false;
    YAMLNodeId g = group ? get_child_by_key(t, e, group) : e;
    if (g == YAML_NULL_ID) return false;
    return get_child_by_key(t, g, param) != YAML_NULL_ID;
}

// Walk `expanded` and `full_expanded` in step and check that every scalar the
// two share holds the same text. Maps are matched by key and sequences by
// index: pruning only ever drops a keyed entry or trims the tail of a line, so
// a node that survives is still where it was. `compared` counts the scalars
// actually checked, which is what keeps the walk from passing vacuously.
void require_same_values(YAMLTreeHandle a, YAMLNodeId na, YAMLTreeHandle b,
                         YAMLNodeId nb, const std::string& path,
                         size_t& compared) {
    if (na == YAML_NULL_ID || nb == YAML_NULL_ID) return;

    // `is_scalar` is ryml's is_val(), which holds only for a sequence entry --
    // every element parameter is a keyval and reports false. A node that is
    // neither a map nor a sequence carries a value, whether or not it has a key.
    if (!is_map(a, na) && !is_sequence(a, na)) {
        // A pruned tree must never hold a *different* value from the bookkept
        // one -- sets and controllers are applied to both, because both are cut
        // from the same finished tree.
        if (is_map(b, nb) || is_sequence(b, nb)) return;
        char* va = as_string(a, na);
        char* vb = as_string(b, nb);
        std::string sa(va ? va : ""), sb(vb ? vb : "");
        yaml_free_string(va);
        yaml_free_string(vb);
        INFO("at " << path);
        REQUIRE(sa == sb);
        ++compared;
        return;
    }

    if (is_map(a, na)) {
        for (size_t i = 0; i < get_size(a, na); ++i) {
            YAMLNodeId ca = get_child_by_index(a, na, i);
            char* k = get_node_key(a, ca);
            if (!k) continue;
            std::string key(k);
            yaml_free_string(k);
            require_same_values(a, ca, b, get_child_by_key(b, nb, key.c_str()),
                                path + "/" + key, compared);
        }
        return;
    }

    if (is_sequence(a, na)) {
        for (size_t i = 0; i < get_size(a, na) && i < get_size(b, nb); ++i)
            require_same_values(a, get_child_by_index(a, na, i), b,
                                get_child_by_index(b, nb, i),
                                path + "/" + std::to_string(i), compared);
    }
}

// The `line` sequence of the first branch of the lattice held by `t`.
YAMLNodeId first_line(YAMLTreeHandle t) {
    YAMLNodeId lat1 = get_child_by_index(t, get_root(t), 0);
    YAMLNodeId branches = get_child_by_key(t, lat1, "branches");
    YAMLNodeId branch = get_child_by_index(t, get_child_by_index(t, branches, 0), 0);
    return get_child_by_key(t, branch, "line");
}

}  // namespace

TEST_CASE("expanded drops the parameters the bookkeeper computed",
          "[expanded]") {
    struct lattices lat = expand_PALS_string(SPLIT_YAML, nullptr);

    // Reference, floor, s-position and element index are computed for every
    // element; none of them survives, and on an element that authored nothing
    // they leave no trace at all.
    for (const char* ele : {"q1", "rf1", "mark"}) {
        REQUIRE(find_by_key(lat.expanded, ele) != YAML_NULL_ID);
        REQUIRE(has_param(lat.full_expanded, ele, "ReferenceP", "pc_ref"));
        REQUIRE(has_param(lat.full_expanded, ele, nullptr, "element_index"));
        REQUIRE_FALSE(has_param(lat.expanded, ele, nullptr, "ReferenceP"));
        REQUIRE_FALSE(has_param(lat.expanded, ele, nullptr, "FloorP"));
        REQUIRE_FALSE(has_param(lat.expanded, ele, nullptr, "s_position"));
        REQUIRE_FALSE(has_param(lat.expanded, ele, nullptr, "element_index"));
    }

    // The authored member of a multipole family stays; the three derived from
    // it go. The group itself stays, because the author wrote it.
    REQUIRE(has_param(lat.expanded, "q1", "MagneticMultipoleP", "Kn1"));
    for (const char* p : {"Kn1L", "Bn1", "Bn1L"}) {
        REQUIRE(has_param(lat.full_expanded, "q1", "MagneticMultipoleP", p));
        REQUIRE_FALSE(has_param(lat.expanded, "q1", "MagneticMultipoleP", p));
    }

    // Ungrouped parameters the author wrote are untouched.
    REQUIRE(has_param(lat.expanded, "q1", nullptr, "length"));
    REQUIRE(has_param(lat.expanded, "q1", nullptr, "kind"));

    free_all(lat);
}

TEST_CASE("expanded keeps an authored group but not what completes it",
          "[expanded]") {
    struct lattices lat = expand_PALS_string(SPLIT_YAML, nullptr);

    // The author wrote ReferenceP on the BeginningEle, so it survives -- with
    // only the two members that were written. `E_tot_ref` and `time_ref` are
    // the bookkeeper completing it, and go. This is the case a "what did the
    // bookkeeper write" list gets wrong: set_num_child overwrites `pc_ref` in
    // place rather than creating it.
    REQUIRE(has_param(lat.expanded, "begin", nullptr, "ReferenceP"));
    REQUIRE(has_param(lat.expanded, "begin", "ReferenceP", "species_ref"));
    REQUIRE(has_param(lat.expanded, "begin", "ReferenceP", "pc_ref"));
    REQUIRE(has_param(lat.full_expanded, "begin", "ReferenceP", "E_tot_ref"));
    REQUIRE_FALSE(has_param(lat.expanded, "begin", "ReferenceP", "E_tot_ref"));
    REQUIRE_FALSE(has_param(lat.expanded, "begin", "ReferenceP", "time_ref"));

    // Which parameters are held is decided by what the author wrote; the value
    // held is the finished one. Here they coincide -- the bookkeeper rewrote
    // `pc_ref` in place with a value equal to the input.
    YAMLNodeId begin = find_by_key(lat.expanded, "begin");
    YAMLNodeId rp = get_child_by_key(lat.expanded, begin, "ReferenceP");
    REQUIRE(close_rel(num_val(lat.expanded, get_child_by_key(lat.expanded, rp,
                                                             "pc_ref")),
                      1.0e9));

    free_all(lat);
}

TEST_CASE("expanded drops materialized group defaults", "[expanded]") {
    struct lattices lat = expand_PALS_string(SPLIT_YAML, nullptr);

    // RFP is authored, so it stays -- holding the gradient and nothing else.
    // `cavity_type` and `zero_phase` are non-zero defaults the bookkeeper fills
    // in, `L_active` defaults to the element length, and `voltage` is derived
    // from the gradient.
    REQUIRE(has_param(lat.expanded, "rf1", "RFP", "gradient"));
    for (const char* p : {"cavity_type", "zero_phase", "L_active", "voltage"}) {
        REQUIRE(has_param(lat.full_expanded, "rf1", "RFP", p));
        REQUIRE_FALSE(has_param(lat.expanded, "rf1", "RFP", p));
    }

    free_all(lat);
}

TEST_CASE("expanded drops the branch_end the bookkeeper appends", "[expanded]") {
    struct lattices lat = expand_PALS_string(SPLIT_YAML, nullptr);

    // The Placeholder capping the branch exists only to carry the branch's
    // final reference and floor, so it has no reason to appear at all -- the
    // line is left one element shorter than the bookkept one.
    REQUIRE(find_by_key(lat.full_expanded, "branch_end") != YAML_NULL_ID);
    REQUIRE(find_by_key(lat.expanded, "branch_end") == YAML_NULL_ID);

    REQUIRE(get_size(lat.expanded, first_line(lat.expanded)) == 4);
    REQUIRE(get_size(lat.full_expanded, first_line(lat.full_expanded)) == 5);

    free_all(lat);
}

TEST_CASE("expanded keeps what a post_expansion set wrote", "[expanded]") {
    struct lattices lat = expand_PALS_string(POST_SET_YAML, nullptr);

    // The set runs after the first bookkeeper pass, so it lands after the point
    // the authored parameters were recorded. Its write is an input all the same
    // and is held; `Kn2`, which the second pass derives from it, is not.
    REQUIRE(has_param(lat.expanded, "q1", "MagneticMultipoleP", "Kn2L"));
    REQUIRE(has_param(lat.full_expanded, "q1", "MagneticMultipoleP", "Kn2"));
    REQUIRE_FALSE(has_param(lat.expanded, "q1", "MagneticMultipoleP", "Kn2"));

    YAMLNodeId q1 = find_by_key(lat.expanded, "q1");
    YAMLNodeId mp = get_child_by_key(lat.expanded, q1, "MagneticMultipoleP");
    REQUIRE(close(
        num_val(lat.expanded, get_child_by_key(lat.expanded, mp, "Kn2L")), 0.75));

    // The set invalidates the rest of the family, which the second bookkeeper
    // pass then rebuilds -- rebuilt values are computed, so none of them stays.
    // `Kn1`, written by the author and untouched by the set, does.
    REQUIRE(has_param(lat.expanded, "q1", "MagneticMultipoleP", "Kn1"));
    REQUIRE_FALSE(has_param(lat.expanded, "q1", "MagneticMultipoleP", "Kn1L"));

    free_all(lat);
}

TEST_CASE("expanded drops the fork wiring but keeps the fork's own inputs",
          "[expanded]") {
    struct lattices lat = expand_PALS_string(FORK_YAML, nullptr);

    // What the author wrote on the Fork stays.
    for (const char* p : {"to_line", "destination_element", "new_branch"}) {
        REQUIRE(has_param(lat.expanded, "to_dump", "ForkP", p));
    }

    // `destination_pointer` is synthesised while the lattice is expanded, before
    // the bookkeeper, so it survives -- and it is still the id of the
    // destination element in this tree, which pruning must not have disturbed
    // (ryml keeps the ids of the nodes it does not remove).
    YAMLNodeId fork = find_by_key(lat.expanded, "to_dump");
    YAMLNodeId fp = get_child_by_key(lat.expanded, fork, "ForkP");
    YAMLNodeId ptr = get_child_by_key(lat.expanded, fp, "destination_pointer");
    REQUIRE(ptr != YAML_NULL_ID);
    char* s = as_string(lat.expanded, ptr);
    REQUIRE(s != nullptr);
    YAMLNodeId dest = (YAMLNodeId)std::strtoull(s, nullptr, 10);
    yaml_free_string(s);
    REQUIRE(key_eq(lat.expanded, dest, "dump_begin"));

    // `propagate_reference` is not written here, so it arrives as a materialized
    // default and goes -- unlike a lattice that writes it, where it stays.
    REQUIRE(has_param(lat.full_expanded, "to_dump", "ForkP",
                      "propagate_reference"));
    REQUIRE_FALSE(has_param(lat.expanded, "to_dump", "ForkP",
                            "propagate_reference"));

    // link_fork_connections runs after the bookkeeper, so what it records is
    // computed too: the name it puts on the Fork and the back-reference it
    // hangs on the destination.
    REQUIRE(has_param(lat.full_expanded, "to_dump", "ForkP", "forked_to"));
    REQUIRE_FALSE(has_param(lat.expanded, "to_dump", "ForkP", "forked_to"));
    REQUIRE(has_param(lat.full_expanded, "dump_begin", nullptr, "ForkFromP"));
    REQUIRE_FALSE(has_param(lat.expanded, "dump_begin", nullptr, "ForkFromP"));

    // The forked branch itself is a structural result of expansion and stays,
    // with its elements.
    REQUIRE(find_by_key(lat.expanded, "this_dump") != YAML_NULL_ID);
    REQUIRE(find_by_key(lat.expanded, "amarker") != YAML_NULL_ID);

    free_all(lat);
}

TEST_CASE("a parameter in both trees holds the same value in both",
          "[expanded]") {
    // The whole invariant, checked wholesale: `expanded` is a pruned copy of the
    // finished lattice, not a snapshot of an earlier one, so a parameter that
    // survives pruning carries the value the lattice ended up with.
    const char* cases[] = {SPLIT_YAML, POST_SET_YAML, FORK_YAML,
                           REWRITTEN_YAML};

    for (const char* yaml : cases) {
        struct lattices lat = expand_PALS_string(yaml, nullptr);
        size_t compared = 0;
        require_same_values(lat.expanded, get_root(lat.expanded),
                            lat.full_expanded, get_root(lat.full_expanded), "",
                            compared);
        REQUIRE(compared > 5);  // not vacuous
        free_all(lat);
    }
}

TEST_CASE("expanded carries the value a controller drove, not the authored one",
          "[expanded]") {
    struct lattices lat =
        expand_PALS_string(REWRITTEN_YAML, nullptr);

    // The ABSOLUTE controller overwrites the authored Kn1L (0.33) with 0.8. The
    // parameter is the author's -- it is held -- but the value is the driven
    // one, matching full_expanded.
    REQUIRE(has_param(lat.expanded, "Qa1", "MagneticMultipoleP", "Kn1L"));
    YAMLNodeId q = find_by_key(lat.expanded, "Qa1");
    YAMLNodeId mp = get_child_by_key(lat.expanded, q, "MagneticMultipoleP");
    REQUIRE(close(
        num_val(lat.expanded, get_child_by_key(lat.expanded, mp, "Kn1L")), 0.8));

    // Likewise the post_expansion set overwrites the authored length.
    REQUIRE(close(
        num_val(lat.expanded,
                get_child_by_key(lat.expanded, q, "length")),
        0.75));

    free_all(lat);
}

TEST_CASE("expanded and full_expanded are independent trees", "[expanded]") {
    struct lattices lat = expand_PALS_string(SPLIT_YAML, nullptr);

    // Both are cut from the same work tree rather than one from the other, so
    // releasing one must leave the other intact.
    delete_tree(lat.expanded);
    lat.expanded = nullptr;

    REQUIRE(has_param(lat.full_expanded, "q1", "MagneticMultipoleP", "Kn1L"));
    REQUIRE(find_by_key(lat.full_expanded, "branch_end") != YAML_NULL_ID);

    free_all(lat);
}
