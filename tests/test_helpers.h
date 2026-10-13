#pragma once

// Shared helpers for the split test files. Definitions are `inline` so the
// header can be included from every test translation unit without ODR clashes.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../src/PALSParserCpp.h"

// ─── file helpers ───────────────────────────────────────────────────────────
// The tests do not create lattice files. A lattice a test needs only in order to
// expand it is held in a string and expanded with expand_PALS_string, and the
// few that are about files on disk -- what `include` and `load` resolve
// against, what parse_file does -- read the fixtures committed under
// tests/lattices.
//
// `name` is resolved against this header's own location, so the fixtures are
// found whatever directory the test binary was started from.
inline std::string lattice_file(const std::string& name) {
    return (std::filesystem::path(__FILE__).parent_path() / "lattices" / name)
        .string();
}

// The text of a fixture, for a test that expands the same document both from
// the file and from a string.
inline std::string read_lattice(const std::string& name) {
    std::ifstream f(lattice_file(name));
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// A path for the tests that exercise the writer to write to. Under the system
// temp directory: nothing the tests produce belongs in the source tree.
inline std::string out_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

inline void remove_file(const std::string& path) {
    std::remove(path.c_str());
}

// Convenience: get a string value and compare, then free it.
inline bool val_eq(YAMLTreeHandle tree, YAMLNodeId node, const char* expected) {
    char* s = as_string(tree, node);
    if (!s) return false;
    bool ok = std::string(s) == expected;
    yaml_free_string(s);
    return ok;
}

// Convenience: compare a node's key, then free it.
inline bool key_eq(YAMLTreeHandle tree, YAMLNodeId node, const char* expected) {
    char* s = get_node_key(tree, node);
    if (!s) return false;
    bool ok = std::string(s) == expected;
    yaml_free_string(s);
    return ok;
}

// ─── expanded-tree navigation ───────────────────────────────────────────────
// Helper: navigate root -> PALS -> facility for a given tree.
inline YAMLNodeId facility_of(YAMLTreeHandle t) {
    YAMLNodeId pals = get_child_by_key(t, get_root(t), "PALS");
    return get_child_by_key(t, pals, "facility");
}

// Navigate root -> PALS -> facility -> the value node keyed `name` inside the
// facility entry whose single key is `entry` (facility is a sequence of
// single-key maps).
inline YAMLNodeId facility_param(YAMLTreeHandle t, const char* entry) {
    YAMLNodeId fac = facility_of(t);
    size_t n = get_size(t, fac);
    for (size_t i = 0; i < n; i++) {
        YAMLNodeId e = get_child_by_index(t, fac, i);
        YAMLNodeId c = get_child_by_key(t, e, entry);
        if (c != YAML_NULL_ID) return c;
    }
    return YAML_NULL_ID;
}

inline double num_val(YAMLTreeHandle t, YAMLNodeId n) {
    char* s = as_string(t, n);
    double v = s ? std::strtod(s, nullptr) : NAN;
    yaml_free_string(s);
    return v;
}

// The first node keyed `key` anywhere in `t`, found depth-first. Used to reach
// an element that expansion inlined into the lattice, wherever it ended up.
inline YAMLNodeId find_by_key(YAMLTreeHandle t, const char* key) {
    std::vector<YAMLNodeId> stack{get_root(t)};
    while (!stack.empty()) {
        YAMLNodeId n = stack.back();
        stack.pop_back();
        char* k = get_node_key(t, n);
        bool hit = k && std::string(k) == key;
        yaml_free_string(k);
        if (hit) return n;
        for (size_t i = 0; i < get_size(t, n); i++)
            stack.push_back(get_child_by_index(t, n, i));
    }
    return YAML_NULL_ID;
}

// ─── float comparison ───────────────────────────────────────────────────────
// True if two doubles agree to a relative/absolute tolerance. The max(1.0, ...)
// floors the tolerance at 1e-9 absolute, which keeps comparisons against zero
// workable but makes this useless for values much smaller than 1e-9 — every
// such comparison passes. Use close_rel for those.
inline bool close(double got, double want) {
    return std::fabs(got - want) <= 1e-9 * std::max(1.0, std::fabs(want));
}

// Purely relative comparison, for quantities whose magnitude is far from 1.
inline bool close_rel(double got, double want) {
    return std::fabs(got - want) <= 1e-9 * std::fabs(want);
}

// ─── shared matching fixture ────────────────────────────────────────────────
// A small lattice covering constants, variables, a controller, elements with
// ungrouped (length) and grouped (BendP.e1) parameters and MetaP aliases, a
// sub-line (sub/S1) used inside another line, and an element (B1a) used twice.
// Expanded, the branch `main` of `lat1` is
//
//   1 start, 2 B1a, 3 B1b, 4 Q1, 5 S1, 6 B1a, 7 branch_end
//
// and `lat2` is the other lattice, which is not the one expanded.
inline const char* MATCH_YAML =
    "PALS:\n"
    "  facility:\n"
    "    - constants:\n"
    "        - a_const: 0.3 * r_electron\n"
    "        - a_two: 5\n"
    "    - my_var:\n"
    "        kind: variable\n"
    "        value: 37\n"
    "    - ps1:\n"
    "        kind: Controller\n"
    "        variables:\n"
    "          cur: 0.1\n"
    "        controls:\n"
    "          - parameter: Q1>MagneticMultipoleP.Kn1\n"
    "            expression: 2*cur\n"
    "    - start:\n"
    "        kind: BeginningEle\n"
    "        ReferenceP:\n"
    "          species_ref: electron\n"
    "          E_tot_ref: 1e9\n"
    "    - B1a:\n"
    "        kind: Bend\n"
    "        length: 1.2\n"
    "        BendP:\n"
    "          e1: 0.1\n"
    "          g_ref: 0.02\n"
    "        MetaP:\n"
    "          alias: black\n"
    "    - B1b:\n"
    "        kind: Bend\n"
    "        length: 1.5\n"
    "        BendP:\n"
    "          e1: 0.3\n"
    "    - Q1:\n"
    "        kind: Quadrupole\n"
    "        length: 0.5\n"
    "        MetaP:\n"
    "          alias: black\n"
    "    - sub:\n"
    "        kind: BeamLine\n"
    "        line:\n"
    "          - S1:\n"
    "              kind: Sextupole\n"
    "              length: 0.2\n"
    "    - main:\n"
    "        kind: BeamLine\n"
    "        line:\n"
    "          - start\n"
    "          - B1a\n"
    "          - B1b\n"
    "          - Q1\n"
    "          - sub\n"
    "          - B1a\n"
    "    - lat2:\n"
    "        kind: Lattice\n"
    "        branches:\n"
    "          - main\n"
    "    - lat1:\n"
    "        kind: Lattice\n"
    "        branches:\n"
    "          - main\n";

// The lattice trees of a document held in a string, freed together.
struct Expanded {
    struct lattices lat;
    explicit Expanded(const char* doc, const char* root = nullptr)
        : lat(expand_PALS_string(doc, root)) {}
    explicit Expanded(const std::string& doc, const char* root = nullptr)
        : Expanded(doc.c_str(), root) {}
    ~Expanded() {
        delete_tree(lat.original);
        delete_tree(lat.combined);
        delete_tree(lat.expanded);
        delete_tree(lat.full_expanded);
        delete_tree(lat.adjunct);
        free_lattice_problems(lat.problems);
    }
    YAMLTreeHandle full() const { return lat.full_expanded; }
};
