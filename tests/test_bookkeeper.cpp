#include "test_helpers.h"

#include <cmath>

// ============================================================
// ELEMENT BOOKKEEPER
// ============================================================
//
// The element bookkeeper (run_element_bookkeeper in pals_expand.cpp) walks each
// branch after expression evaluation and fills in the position-dependent output
// parameters: reference (species / energy / momentum / time), floor placement,
// s_position, and the field-dependent multipole / bend strengths. The floor math
// lives in the quaternion module pals_floor.cpp. These tests pin the values it
// produces for a small branch with a drift, a quadrupole, and a bend.

namespace {

// A branch: BeginningEle (1 GeV electrons) -> 2 m drift -> 0.5 m quad with a
// normal quadrupole field -> 1.5 m bend of 0.15 rad -> 1 m drift -> Marker.
const char* BOOKKEEPER_YAML =
    "PALS:\n"
    "  facility:\n"
    "    - test_line:\n"
    "        kind: BeamLine\n"
    "        line:\n"
    "          - begin:\n"
    "              kind: BeginningEle\n"
    "              ReferenceP:\n"
    "                species_ref: \"electron\"\n"
    "                E_tot_ref: 1.0e9\n"
    "          - d1:\n"
    "              kind: Drift\n"
    "              length: 2.0\n"
    "          - q1:\n"
    "              kind: Quadrupole\n"
    "              length: 0.5\n"
    "              MagneticMultipoleP:\n"
    "                Bn1: 1.2\n"
    "          - b1:\n"
    "              kind: Bend\n"
    "              length: 1.5\n"
    "              BendP:\n"
    "                angle_ref: 0.15\n"
    "          - d2:\n"
    "              kind: Drift\n"
    "              length: 1.0\n"
    "          - end:\n"
    "              kind: Marker\n"
    "    - lat:\n"
    "        kind: Lattice\n"
    "        branches:\n"
    "          - test_line\n"
    "    - use: lat\n";

// Value of ele>group.param in the expanded tree, as a double.
double param_num(YAMLTreeHandle t, const char* ele, const char* group,
                 const char* param) {
    YAMLNodeId e = find_by_key(t, ele);
    YAMLNodeId g = group ? get_child_by_key(t, e, group) : e;
    return num_val(t, get_child_by_key(t, g, param));
}

// Expand BOOKKEEPER_YAML. The caller frees the returned trees and problem list.
struct lattices expand_bookkeeper() {
    return expand_PALS_string(BOOKKEEPER_YAML, nullptr);
}

}  // namespace

TEST_CASE("Bookkeeper fills reference energy, momentum, and time",
          "[bookkeeper]") {
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;

    // pc = sqrt(E^2 - m^2) with the electron rest energy ~0.511 MeV; every
    // element downstream carries the same species and (drifts don't change it)
    // energy/momentum.
    const double m_e = 510998.95;  // eV, apc electron rest energy
    const double pc = std::sqrt(1.0e9 * 1.0e9 - m_e * m_e);
    for (const char* ele : {"begin", "d1", "q1", "b1", "d2", "end"}) {
        REQUIRE(close_rel(param_num(t, ele, "ReferenceP", "pc_ref"), pc));
        REQUIRE(close_rel(param_num(t, ele, "ReferenceP", "E_tot_ref"), 1.0e9));
    }

    // Reference time is zero at the start and grows by length / (beta c) across
    // each element. beta = pc / E.
    const double c = 2.99792458e8;
    const double beta = pc / 1.0e9;
    REQUIRE(close(param_num(t, "begin", "ReferenceP", "time_ref"), 0.0));
    REQUIRE(close_rel(param_num(t, "q1", "ReferenceP", "time_ref"),
                      2.0 / (beta * c)));               // after the 2 m drift
    REQUIRE(close_rel(param_num(t, "end", "ReferenceP", "time_ref"),
                      5.0 / (beta * c)));               // total path length 5 m

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper accumulates s_position from element lengths",
          "[bookkeeper]") {
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;

    // s_position is the upstream end of each element: 0, then the running sum of
    // the preceding element lengths (0, 2, 0.5, 1.5, 1).
    REQUIRE(close(param_num(t, "begin", nullptr, "s_position"), 0.0));
    REQUIRE(close(param_num(t, "d1", nullptr, "s_position"), 0.0));
    REQUIRE(close(param_num(t, "q1", nullptr, "s_position"), 2.0));
    REQUIRE(close(param_num(t, "b1", nullptr, "s_position"), 2.5));
    REQUIRE(close(param_num(t, "d2", nullptr, "s_position"), 4.0));
    REQUIRE(close(param_num(t, "end", nullptr, "s_position"), 5.0));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper derives the normalized multipole strength", "[bookkeeper]") {
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;

    // Kn1 = (q / P0) Bn1 = (charge * c_light / pc) * Bn1, with charge = -1 for
    // the electron, referenced to q1's upstream momentum.
    const double m_e = 510998.95;
    const double pc = std::sqrt(1.0e9 * 1.0e9 - m_e * m_e);
    const double factor = -1.0 * 2.99792458e8 / pc;
    REQUIRE(close_rel(param_num(t, "q1", "MagneticMultipoleP", "Kn1"),
                      factor * 1.2));
    // The user-supplied field value is left as written.
    REQUIRE(close(param_num(t, "q1", "MagneticMultipoleP", "Bn1"), 1.2));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper caps each branch with a branch_end Placeholder",
          "[bookkeeper]") {
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;

    // The branch gains a trailing element named `branch_end` of kind Placeholder.
    YAMLNodeId be = find_by_key(t, "branch_end");
    REQUIRE(be != YAML_NULL_ID);
    REQUIRE(val_eq(t, get_child_by_key(t, be, "kind"), "Placeholder"));

    // It holds the branch's final state: the downstream end of the last element.
    // Here the last real element is the zero-length Marker `end`, so branch_end's
    // placement, reference, and s-position match it exactly.
    REQUIRE(close(param_num(t, "branch_end", nullptr, "s_position"),
                  param_num(t, "end", nullptr, "s_position")));
    REQUIRE(close(param_num(t, "branch_end", "FloorP", "z"),
                  param_num(t, "end", "FloorP", "z")));
    REQUIRE(close(param_num(t, "branch_end", "FloorP", "theta"), -0.15));

    const double m_e = 510998.95;
    const double pc = std::sqrt(1.0e9 * 1.0e9 - m_e * m_e);
    REQUIRE(close_rel(param_num(t, "branch_end", "ReferenceP", "pc_ref"), pc));
    REQUIRE(val_eq(t, get_child_by_key(t, get_child_by_key(t, be, "ReferenceP"),
                                       "species_ref"),
                   "electron"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper numbers each element with its position in the branch",
          "[bookkeeper]") {
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;

    // element_index counts from one along the branch line, so it is the array
    // index of the element rather than an offset. The `branch_end` cap is an
    // element of the line like any other and takes the next number after the
    // last real one.
    const char* order[] = {"begin", "d1", "q1", "b1", "d2", "end", "branch_end"};
    for (size_t i = 0; i < 7; ++i)
        REQUIRE(param_num(t, order[i], nullptr, "element_index") ==
                static_cast<double>(i + 1));

    // It is written as a plain integer, not through the double formatter that
    // gives the physical parameters their shortest round-tripping decimal.
    REQUIRE(val_eq(t, get_child_by_key(t, find_by_key(t, "q1"), "element_index"),
                   "3"));

    // Being computed, it belongs to `full_expanded` alone; `expanded` carries
    // what the author wrote.
    REQUIRE(get_child_by_key(lat.expanded, find_by_key(lat.expanded, "q1"),
                             "element_index") == YAML_NULL_ID);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("element_index restarts at one in each branch", "[bookkeeper]") {
    // A Fork gives the lattice a second branch. The index is a position within
    // the branch that holds the element, not a running count over the lattice,
    // so the forked branch starts again at one.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - dump_begin:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: \"electron\"\n"
        "          E_tot_ref: 1.0e9\n"
        "    - dump_end:\n"
        "        kind: Marker\n"
        "    - dump_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - dump_begin\n"
        "          - dump_end\n"
        "    - ring:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin:\n"
        "              kind: BeginningEle\n"
        "              ReferenceP:\n"
        "                species_ref: \"electron\"\n"
        "                E_tot_ref: 1.0e9\n"
        "          - drift1:\n"
        "              kind: Drift\n"
        "              length: 1.0\n"
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
    YAMLTreeHandle t = lat.full_expanded;

    // ring: begin, drift1, f1, branch_end.
    REQUIRE(param_num(t, "begin", nullptr, "element_index") == 1.0);
    REQUIRE(param_num(t, "drift1", nullptr, "element_index") == 2.0);
    REQUIRE(param_num(t, "f1", nullptr, "element_index") == 3.0);

    // dump_line: numbered from one again, and capped by its own branch_end.
    REQUIRE(param_num(t, "dump_begin", nullptr, "element_index") == 1.0);
    REQUIRE(param_num(t, "dump_end", nullptr, "element_index") == 2.0);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// A branch with a single RFCavity, whose RFP body is spliced in from `rfp_body`
// (raw YAML at 16-space indentation). Lets each RF test set voltage/gradient/
// L_active as it likes.
static std::string rf_yaml(const std::string& rfp_body) {
    return std::string(
               "PALS:\n"
               "  facility:\n"
               "    - test_line:\n"
               "        kind: BeamLine\n"
               "        line:\n"
               "          - begin:\n"
               "              kind: BeginningEle\n"
               "              ReferenceP:\n"
               "                species_ref: \"electron\"\n"
               "                E_tot_ref: 1.0e9\n"
               "          - cav:\n"
               "              kind: RFCavity\n"
               "              length: 0.4\n"
               "              RFP:\n") +
           rfp_body +
           "    - lat:\n"
           "        kind: Lattice\n"
           "        branches:\n"
           "          - test_line\n"
           "    - use: lat\n";
}

static struct lattices expand_rf(const std::string& rfp_body) {
    const std::string doc = rf_yaml(rfp_body);
    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    return lat;
}

static bool any_problem_contains(struct lattices& lat, const char* needle) {
    for (size_t i = 0; i < lat.problems.count; ++i)
        if (std::string(lat.problems.items[i].message).find(needle) != std::string::npos)
            return true;
    return false;
}

// A branch: BeginningEle (1 GeV electrons) followed by a single element whose
// definition is spliced in from `ele_body` (raw YAML at 10-space indentation,
// e.g. "          - el:\n              kind: Bend\n ..."). Lets a test drive one
// element's dependent parameters in isolation.
static std::string one_ele_yaml(const std::string& ele_body) {
    return std::string(
               "PALS:\n"
               "  facility:\n"
               "    - test_line:\n"
               "        kind: BeamLine\n"
               "        line:\n"
               "          - begin:\n"
               "              kind: BeginningEle\n"
               "              ReferenceP:\n"
               "                species_ref: \"electron\"\n"
               "                E_tot_ref: 1.0e9\n") +
           ele_body +
           "    - lat:\n"
           "        kind: Lattice\n"
           "        branches:\n"
           "          - test_line\n"
           "    - use: lat\n";
}

static struct lattices expand_src(const std::string& src) {
    const std::string doc = src;
    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    return lat;
}

// The reference momentum and the field<->normalized conversion factor for a
// 1 GeV electron (charge -1), shared by the multipole / bend-field tests.
static double electron_pc() {
    const double m_e = 510998.95;  // eV, apc electron rest energy
    return std::sqrt(1.0e9 * 1.0e9 - m_e * m_e);
}
static double electron_factor() { return -1.0 * 2.99792458e8 / electron_pc(); }

TEST_CASE("Bookkeeper derives RF voltage from gradient and active length",
          "[bookkeeper]") {
    // Only the gradient is given; L_active defaults to the element length 0.4, so
    // voltage = gradient * L_active = 1e8 * 0.4 = 4e7. No inconsistency reported.
    struct lattices lat = expand_rf("                gradient: 1.0e8\n");
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close_rel(param_num(t, "cav", "RFP", "voltage"), 4.0e7));
    REQUIRE(close(param_num(t, "cav", "RFP", "gradient"), 1.0e8));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper derives RF gradient from voltage and active length",
          "[bookkeeper]") {
    // Only the voltage is given; gradient = voltage / L_active = 8e6 / 0.4 = 2e7.
    struct lattices lat = expand_rf("                voltage: 8.0e6\n");
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close_rel(param_num(t, "cav", "RFP", "gradient"), 2.0e7));
    REQUIRE(close(param_num(t, "cav", "RFP", "voltage"), 8.0e6));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper accepts consistent RF voltage and gradient",
          "[bookkeeper]") {
    // Both set and agreeing (2e7 * 0.4 = 8e6): no problem, values untouched.
    struct lattices lat =
        expand_rf("                gradient: 2.0e7\n"
                  "                voltage: 8.0e6\n");
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "cav", "RFP", "gradient"), 2.0e7));
    REQUIRE(close(param_num(t, "cav", "RFP", "voltage"), 8.0e6));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper flags inconsistent RF voltage and gradient",
          "[bookkeeper]") {
    // The PALSJulia/alocal case: gradient 1e8 with L_active 0.4 implies voltage
    // 4e7, but voltage is set to 45. That must be reported as inconsistent.
    struct lattices lat =
        expand_rf("                gradient: 1.0e8\n"
                  "                voltage: 45\n");

    REQUIRE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper honors an explicit L_active for RF derivation",
          "[bookkeeper]") {
    // L_active (0.25) differs from the element length (0.4); the derived voltage
    // follows the active length: 1e8 * 0.25 = 2.5e7.
    struct lattices lat =
        expand_rf("                gradient: 1.0e8\n"
                  "                L_active: 0.25\n");
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close_rel(param_num(t, "cav", "RFP", "voltage"), 2.5e7));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper propagates floor coordinates through a bend",
          "[bookkeeper]") {
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;

    // Everything up to the bend lies along +Z at the origin, unrotated.
    REQUIRE(close(param_num(t, "b1", "FloorP", "z"), 2.5));
    REQUIRE(close(param_num(t, "b1", "FloorP", "x"), 0.0));
    REQUIRE(close(param_num(t, "b1", "FloorP", "theta"), 0.0));

    // The bend has arc length 1.5 and angle 0.15, so rho = 10. Its downstream
    // frame (= d2's upstream frame) is displaced by (rho(cos a - 1), 0,
    // rho sin a) from the bend entrance at z = 2.5, and a positive bend angle
    // decreases theta.
    const double rho = 1.5 / 0.15;
    REQUIRE(close(param_num(t, "d2", "FloorP", "x"), rho * (std::cos(0.15) - 1.0)));
    REQUIRE(close(param_num(t, "d2", "FloorP", "z"), 2.5 + rho * std::sin(0.15)));
    REQUIRE(close(param_num(t, "d2", "FloorP", "theta"), -0.15));

    // The final 1 m drift advances along the rotated z-axis (azimuth -0.15).
    REQUIRE(close(param_num(t, "end", "FloorP", "theta"), -0.15));
    REQUIRE(close(param_num(t, "end", "FloorP", "x"),
                  param_num(t, "d2", "FloorP", "x") + 1.0 * std::sin(-0.15)));
    REQUIRE(close(param_num(t, "end", "FloorP", "z"),
                  param_num(t, "d2", "FloorP", "z") + 1.0 * std::cos(-0.15)));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper derives all bend strength forms from the angle",
          "[bookkeeper]") {
    // b1 gives only angle_ref 0.15 over length 1.5. The expanded lattice holds
    // every equivalent form: g_ref = angle/length = 0.1, radius_ref = 1/g_ref =
    // 10, and Bn0_ref = g_ref / factor (factor = -c/pc for the electron).
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "b1", "BendP", "angle_ref"), 0.15));
    REQUIRE(close(param_num(t, "b1", "BendP", "g_ref"), 0.1));
    REQUIRE(close(param_num(t, "b1", "BendP", "radius_ref"), 10.0));
    REQUIRE(close_rel(param_num(t, "b1", "BendP", "Bn0_ref"),
                      0.1 / electron_factor()));

    // ...and the lengths that go with that shape (bend.md): the chord between
    // the entrance and exit origins, its projection on the entrance axis, and
    // the sagitta out to the arc. rho = 10, angle = 0.15.
    REQUIRE(close_rel(param_num(t, "b1", "BendP", "L_chord"),
                      2.0 * 10.0 * std::sin(0.075)));
    REQUIRE(close_rel(param_num(t, "b1", "BendP", "L_rectangle"),
                      10.0 * std::sin(0.15)));
    REQUIRE(close_rel(param_num(t, "b1", "BendP", "L_sagitta"),
                      10.0 * (1.0 - std::cos(0.075))));

    // The non-zero enum defaults of the present BendP group are made explicit.
    YAMLNodeId bp = get_child_by_key(t, find_by_key(t, "b1"), "BendP");
    REQUIRE(val_eq(t, get_child_by_key(t, bp, "ref_geometry"), "ARC"));
    REQUIRE(val_eq(t, get_child_by_key(t, bp, "multipole_geometry"),
                   "FOLLOWS_REF_GEOMETRY"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper derives integrated and normalized multipole forms",
          "[bookkeeper]") {
    // q1 gives only Bn1 1.2 over length 0.5. The expanded lattice holds all four
    // equivalent forms: the field (Bn1), its length integral (Bn1L = L*Bn1), the
    // normalized value (Kn1 = factor*Bn1), and the normalized integral (Kn1L).
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;
    const double f = electron_factor();

    REQUIRE(close(param_num(t, "q1", "MagneticMultipoleP", "Bn1"), 1.2));
    REQUIRE(close(param_num(t, "q1", "MagneticMultipoleP", "Bn1L"), 0.6));
    REQUIRE(close_rel(param_num(t, "q1", "MagneticMultipoleP", "Kn1"), f * 1.2));
    REQUIRE(close_rel(param_num(t, "q1", "MagneticMultipoleP", "Kn1L"), f * 0.6));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper derives the integrated electric multipole", "[bookkeeper]") {
    // Electric multipoles have no normalized form; only length-integration ties
    // En2 to En2L = length * En2 = 0.5 * 5 = 2.5.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - em:\n"
        "              kind: Sextupole\n"
        "              length: 0.5\n"
        "              ElectricMultipoleP:\n"
        "                En2: 5.0\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "em", "ElectricMultipoleP", "En2"), 5.0));
    REQUIRE(close(param_num(t, "em", "ElectricMultipoleP", "En2L"), 2.5));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper derives the bend length from its angle and radius",
          "[bookkeeper]") {
    // The angle and the radius are enough on their own: the arc length follows,
    // and with it the chord, the rectangle and the sagitta. bend.md lets an
    // author give any two of {curvature, a length, the angle}.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              BendP:\n"
        "                angle_ref: 0.2\n"
        "                radius_ref: 5.0\n"
        "          - after:\n"
        "              kind: Marker\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "bnd", nullptr, "length"), 1.0));
    // The derived length carries downstream: the marker sits at the far end.
    REQUIRE(close(param_num(t, "after", nullptr, "s_position"), 1.0));
    REQUIRE(close(param_num(t, "bnd", "BendP", "g_ref"), 0.2));
    REQUIRE(close_rel(param_num(t, "bnd", "BendP", "L_chord"),
                      2.0 * 5.0 * std::sin(0.1)));
    REQUIRE(close_rel(param_num(t, "bnd", "BendP", "L_rectangle"),
                      5.0 * std::sin(0.2)));
    REQUIRE(close_rel(param_num(t, "bnd", "BendP", "L_sagitta"),
                      5.0 * (1.0 - std::cos(0.1))));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper derives the bend angle from its chord", "[bookkeeper]") {
    // L_chord = 2 sin(angle/2) / g_ref inverts to the angle, which then gives
    // the arc length the element never stated.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              BendP:\n"
        "                g_ref: 0.25\n"
        "                L_chord: 1.9\n"));
    YAMLTreeHandle t = lat.full_expanded;
    const double angle = 2.0 * std::asin(0.25 * 1.9 / 2.0);

    REQUIRE(close_rel(param_num(t, "bnd", "BendP", "angle_ref"), angle));
    REQUIRE(close_rel(param_num(t, "bnd", nullptr, "length"), angle / 0.25));
    REQUIRE(close(param_num(t, "bnd", "BendP", "radius_ref"), 4.0));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper gives a straight bend three equal lengths",
          "[bookkeeper]") {
    // No curvature and no angle: the arc, the chord and the rectangle coincide.
    // The sagitta is zero, and a zero is not held.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 0.8\n"
        "              BendP:\n"
        "                e1: 0.01\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "bnd", "BendP", "L_chord"), 0.8));
    REQUIRE(close(param_num(t, "bnd", "BendP", "L_rectangle"), 0.8));
    YAMLNodeId bp = get_child_by_key(t, find_by_key(t, "bnd"), "BendP");
    REQUIRE(get_child_by_key(t, bp, "L_sagitta") == YAML_NULL_ID);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper keeps the sagitta of a nearly straight bend",
          "[bookkeeper]") {
    // The lengths are computed through sinc rather than 1/g_ref, so a bend this
    // gentle is not a special case: angle = 2e-12 and the sagitta is
    // length*angle/8 = 5e-13. Taken as radius*(1 - cos(angle/2)) instead, the
    // subtraction would have cancelled it away to zero.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 2.0\n"
        "              BendP:\n"
        "                g_ref: 1.0e-12\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "bnd", "BendP", "angle_ref"), 2.0e-12));
    REQUIRE(close(param_num(t, "bnd", "BendP", "L_chord"), 2.0));
    REQUIRE(close(param_num(t, "bnd", "BendP", "L_rectangle"), 2.0));
    REQUIRE(close_rel(param_num(t, "bnd", "BendP", "L_sagitta"), 5.0e-13));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper flags a chord that disagrees with the bend geometry",
          "[bookkeeper]") {
    // angle_ref 0.15 over length 1.5 puts the chord at ~1.4986, but L_chord is
    // set to 1.8. The author's value is reported, not overwritten.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 1.5\n"
        "              BendP:\n"
        "                angle_ref: 0.15\n"
        "                L_chord: 1.8\n"));

    REQUIRE(any_problem_contains(lat, "'L_chord' is inconsistent"));
    REQUIRE(close(param_num(lat.full_expanded, "bnd", "BendP", "L_chord"), 1.8));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper flags an inconsistent bend strength", "[bookkeeper]") {
    // angle_ref 0.15 over length 1.5 implies g_ref 0.1, but g_ref is set to 0.2.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 1.5\n"
        "              BendP:\n"
        "                angle_ref: 0.15\n"
        "                g_ref: 0.2\n"));

    REQUIRE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper defaults the bend actual field from g_ref",
          "[bookkeeper]") {
    // Kn0_from_g_ref (bend.md) is true by default, so a bend that says nothing
    // about its actual field bends its own reference particle along the
    // reference curve: Kn0 = g_ref. The element carries no MagneticMultipoleP,
    // so the group is created to hold it, and the other three forms of the
    // dipole component follow as usual.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 2.0\n"
        "              BendP:\n"
        "                g_ref: 0.1\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "bnd", "MagneticMultipoleP", "Kn0"), 0.1));
    REQUIRE(close(param_num(t, "bnd", "MagneticMultipoleP", "Kn0L"), 0.2));
    REQUIRE(close_rel(param_num(t, "bnd", "MagneticMultipoleP", "Bn0"),
                      0.1 / electron_factor()));
    REQUIRE(close_rel(param_num(t, "bnd", "MagneticMultipoleP", "Bn0L"),
                      0.2 / electron_factor()));
    // The flag itself is a non-false default of a present group, so it is made
    // explicit alongside the enum defaults.
    YAMLNodeId bp = get_child_by_key(t, find_by_key(t, "bnd"), "BendP");
    REQUIRE(val_eq(t, get_child_by_key(t, bp, "Kn0_from_g_ref"), "true"));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper writes no actual field when Kn0_from_g_ref is false",
          "[bookkeeper]") {
    // The author has said the actual field is not the reference one. Nothing is
    // written, and the bend keeps no multipole group at all.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 2.0\n"
        "              BendP:\n"
        "                g_ref: 0.1\n"
        "                Kn0_from_g_ref: false\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(get_child_by_key(t, find_by_key(t, "bnd"),
                             "MagneticMultipoleP") == YAML_NULL_ID);
    // The reference bend is untouched: only the actual field is being declined.
    REQUIRE(close(param_num(t, "bnd", "BendP", "angle_ref"), 0.2));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper keeps an actual bend field the author set",
          "[bookkeeper]") {
    // Kn0 is set to something other than g_ref -- a bend running off its
    // reference field, which is exactly what the parameters are separate for.
    // The default does not apply, and the difference is not an inconsistency.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 2.0\n"
        "              BendP:\n"
        "                g_ref: 0.1\n"
        "              MagneticMultipoleP:\n"
        "                Kn0: 0.3\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "bnd", "MagneticMultipoleP", "Kn0"), 0.3));
    REQUIRE(close(param_num(t, "bnd", "MagneticMultipoleP", "Kn0L"), 0.6));
    REQUIRE(close(param_num(t, "bnd", "BendP", "g_ref"), 0.1));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper counts an integrated actual field as set",
          "[bookkeeper]") {
    // Kn0L states the same field as Kn0, just integrated over the length, so
    // the default does not apply: Kn0 comes from Kn0L / length = 0.3, not from
    // g_ref. A Kn0 of 0.1 here would have contradicted the author's own value.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 2.0\n"
        "              BendP:\n"
        "                g_ref: 0.1\n"
        "              MagneticMultipoleP:\n"
        "                Kn0L: 0.6\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "bnd", "MagneticMultipoleP", "Kn0"), 0.3));
    REQUIRE_FALSE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper adds the actual field to a group of other multipoles",
          "[bookkeeper]") {
    // A different order says nothing about the dipole component: the quadrupole
    // the author gave stays, and Kn0 joins it in the group already there.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 2.0\n"
        "              BendP:\n"
        "                g_ref: 0.1\n"
        "              MagneticMultipoleP:\n"
        "                Kn1: 0.5\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(close(param_num(t, "bnd", "MagneticMultipoleP", "Kn0"), 0.1));
    REQUIRE(close(param_num(t, "bnd", "MagneticMultipoleP", "Kn1"), 0.5));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper gives a straight bend no actual field", "[bookkeeper]") {
    // No curvature, so there is no reference field to copy and nothing to hold:
    // a zero Kn0 is not written, and no group is created for it.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - bnd:\n"
        "              kind: Bend\n"
        "              length: 2.0\n"
        "              BendP:\n"
        "                e1: 0.01\n"));
    YAMLTreeHandle t = lat.full_expanded;

    REQUIRE(get_child_by_key(t, find_by_key(t, "bnd"),
                             "MagneticMultipoleP") == YAML_NULL_ID);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper flags an inconsistent multipole component",
          "[bookkeeper]") {
    // Bn1 1.2 over length 0.5 implies Bn1L 0.6, but Bn1L is set to 0.5.
    struct lattices lat = expand_src(one_ele_yaml(
        "          - sx:\n"
        "              kind: Sextupole\n"
        "              length: 0.5\n"
        "              MagneticMultipoleP:\n"
        "                Bn1: 1.2\n"
        "                Bn1L: 0.5\n"));

    REQUIRE(any_problem_contains(lat, "inconsistent"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper materializes RF enum defaults and L_active",
          "[bookkeeper]") {
    // A present RFP group gains its non-zero enum defaults and an explicit
    // L_active (= element length 0.4), alongside the derived voltage.
    struct lattices lat = expand_rf("                gradient: 1.0e8\n");
    YAMLTreeHandle t = lat.full_expanded;

    YAMLNodeId rfp = get_child_by_key(t, find_by_key(t, "cav"), "RFP");
    REQUIRE(val_eq(t, get_child_by_key(t, rfp, "cavity_type"), "STANDING_WAVE"));
    REQUIRE(val_eq(t, get_child_by_key(t, rfp, "zero_phase"), "ACCELERATING"));
    REQUIRE(close(param_num(t, "cav", "RFP", "L_active"), 0.4));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("Bookkeeper leaves absent parameter groups alone", "[bookkeeper]") {
    // Groups not present in the file are not materialized: a plain drift gains no
    // BendP/RFP/multipole groups. Only ReferenceP and FloorP are parser-added,
    // and MagneticMultipoleP on a curved bend, which has an actual field to hold.
    struct lattices lat = expand_bookkeeper();
    YAMLTreeHandle t = lat.full_expanded;
    YAMLNodeId d1 = find_by_key(t, "d1");

    REQUIRE(get_child_by_key(t, d1, "BendP") == YAML_NULL_ID);
    REQUIRE(get_child_by_key(t, d1, "RFP") == YAML_NULL_ID);
    REQUIRE(get_child_by_key(t, d1, "MagneticMultipoleP") == YAML_NULL_ID);
    REQUIRE(get_child_by_key(t, d1, "ReferenceP") != YAML_NULL_ID);
    REQUIRE(get_child_by_key(t, d1, "FloorP") != YAML_NULL_ID);

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// ------------------------------------------------------------
// Reference energy changes, Patch geometry, and bend lengths
// ------------------------------------------------------------

namespace {

// A branch: BeginningEle (1 GeV electrons) -> 1 m drift `d1` -> `middle` ->
// 1 m drift `d2` -> Marker `end`. `middle` is line entries, indented for `line:`.
std::string line_around(const std::string& middle) {
    return "PALS:\n"
           "  facility:\n"
           "    - ln:\n"
           "        kind: BeamLine\n"
           "        line:\n"
           "          - begin:\n"
           "              kind: BeginningEle\n"
           "              ReferenceP:\n"
           "                species_ref: electron\n"
           "                E_tot_ref: 1.0e9\n"
           "          - d1:\n"
           "              kind: Drift\n"
           "              length: 1.0\n" +
           middle +
           "          - d2:\n"
           "              kind: Drift\n"
           "              length: 1.0\n"
           "          - end:\n"
           "              kind: Marker\n"
           "    - lat:\n"
           "        kind: Lattice\n"
           "        branches:\n"
           "          - ln\n"
           "    - use: lat\n";
}

std::vector<std::string> problem_list(const struct lattices& lat) {
    std::vector<std::string> out;
    for (size_t i = 0; i < lat.problems.count; ++i)
        out.emplace_back(lat.problems.items[i].message);
    return out;
}

std::string problem_text(const struct lattices& lat) {
    std::string s;
    for (const std::string& m : problem_list(lat)) s += m + "; ";
    return s;
}

void free_lat(struct lattices& lat) {
    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

const double M_ELECTRON = 510998.95;  // eV
const double C_LIGHT = 2.99792458e8;  // m/s

double pc_of(double E, double m) { return std::sqrt(E * E - m * m); }

}  // namespace

TEST_CASE("Bookkeeper adds an RFCavity's dE_ref to the reference energy",
          "[bookkeeper][reference]") {
    // rf.md: the downstream reference energy is E_tot_ref + dE_ref. The
    // transit time uses the average of the two ends,
    // length * (E_up + E_down) / (c * (pc_up + pc_down)).
    struct lattices lat = expand_PALS_string(
        line_around("          - cav:\n"
                    "              kind: RFCavity\n"
                    "              length: 0.5\n"
                    "              RFP:\n"
                    "                dE_ref: 1.0e8\n")
            .c_str(),
        nullptr);
    INFO(problem_text(lat));
    YAMLTreeHandle t = lat.full_expanded;
    REQUIRE(close_rel(param_num(t, "cav", "ReferenceP", "E_tot_ref"), 1.0e9));
    REQUIRE(close_rel(param_num(t, "d2", "ReferenceP", "E_tot_ref"), 1.1e9));
    REQUIRE(close_rel(param_num(t, "d2", "ReferenceP", "pc_ref"),
                      pc_of(1.1e9, M_ELECTRON)));
    const double pc0 = pc_of(1.0e9, M_ELECTRON), pc1 = pc_of(1.1e9, M_ELECTRON);
    const double t_cav = param_num(t, "cav", "ReferenceP", "time_ref");
    REQUIRE(close_rel(param_num(t, "d2", "ReferenceP", "time_ref") - t_cav,
                      0.5 * (1.0e9 + 1.1e9) / (C_LIGHT * (pc0 + pc1))));
    free_lat(lat);
}

TEST_CASE("Bookkeeper applies a ReferenceChange's energy settings",
          "[bookkeeper][reference]") {
    // referencechange.md: E_tot_ref and pc_ref set the energy, dE_ref and
    // dpc_ref adjust it. The partner quantity is recomputed.
    struct Case {
        const char* group;
        double E_down;
    };
    const double pc0 = pc_of(1.0e9, M_ELECTRON);
    const double pc_set = 2.0e9;
    const Case cases[] = {
        {"                E_tot_ref: 2.0e9\n", 2.0e9},
        {"                dE_ref: 5.0e8\n", 1.5e9},
        {"                pc_ref: 2.0e9\n",
         std::sqrt(pc_set * pc_set + M_ELECTRON * M_ELECTRON)},
        {"                dpc_ref: 1.0e8\n",
         std::sqrt((pc0 + 1.0e8) * (pc0 + 1.0e8) + M_ELECTRON * M_ELECTRON)},
    };
    for (const Case& c : cases) {
        INFO(c.group);
        struct lattices lat = expand_PALS_string(
            line_around(std::string("          - rc:\n"
                                    "              kind: ReferenceChange\n"
                                    "              ReferenceChangeP:\n") +
                        c.group)
                .c_str(),
            nullptr);
        INFO(problem_text(lat));
        REQUIRE(lat.problems.count == 0);
        YAMLTreeHandle t = lat.full_expanded;
        REQUIRE(close_rel(param_num(t, "rc", "ReferenceP", "E_tot_ref"), 1.0e9));
        REQUIRE(close_rel(param_num(t, "d2", "ReferenceP", "E_tot_ref"), c.E_down));
        REQUIRE(close_rel(param_num(t, "end", "ReferenceP", "pc_ref"),
                          pc_of(c.E_down, M_ELECTRON)));
        free_lat(lat);
    }
}

TEST_CASE("Bookkeeper applies a ReferenceChange's species and time settings",
          "[bookkeeper][reference]") {
    struct lattices lat = expand_PALS_string(
        line_around("          - rc:\n"
                    "              kind: ReferenceChange\n"
                    "              ReferenceChangeP:\n"
                    "                species_ref: proton\n"
                    "                E_tot_ref: 2.0e9\n"
                    "                time_ref: 1.0e-6\n"
                    "          - rc2:\n"
                    "              kind: ReferenceChange\n"
                    "              ReferenceChangeP:\n"
                    "                dtime_ref: 1.0e-9\n")
            .c_str(),
        nullptr);
    INFO(problem_text(lat));
    REQUIRE(lat.problems.count == 0);
    YAMLTreeHandle t = lat.full_expanded;
    YAMLNodeId rp = get_child_by_key(t, find_by_key(t, "d2"), "ReferenceP");
    REQUIRE(val_eq(t, get_child_by_key(t, rp, "species_ref"), "proton"));
    // The momentum is recomputed with the proton mass.
    const double E = param_num(t, "d2", "ReferenceP", "E_tot_ref");
    const double pc = param_num(t, "d2", "ReferenceP", "pc_ref");
    REQUIRE(close_rel(E, 2.0e9));
    REQUIRE(std::fabs(std::sqrt(E * E - pc * pc) / 938.272e6 - 1.0) < 1e-5);
    // time_ref sets the time; dtime_ref adds to it. A ReferenceChange has no
    // length, so the time carries straight on to the next element.
    REQUIRE(close_rel(param_num(t, "rc2", "ReferenceP", "time_ref"), 1.0e-6));
    REQUIRE(close_rel(param_num(t, "d2", "ReferenceP", "time_ref"),
                      1.0e-6 + 1.0e-9));
    free_lat(lat);
}

TEST_CASE("Bookkeeper flags more than one ReferenceChange energy or time setting",
          "[bookkeeper][reference][problems]") {
    // referencechange.md: only one of dE_ref, dpc_ref, E_tot_ref and pc_ref
    // may be present, and only one of dtime_ref and time_ref.
    struct lattices lat = expand_PALS_string(
        line_around("          - rc:\n"
                    "              kind: ReferenceChange\n"
                    "              ReferenceChangeP:\n"
                    "                E_tot_ref: 2.0e9\n"
                    "                dE_ref: 1.0e8\n"
                    "                time_ref: 1.0e-6\n"
                    "                dtime_ref: 1.0e-9\n")
            .c_str(),
        nullptr);
    std::string all = problem_text(lat);
    INFO(all);
    REQUIRE(all.find("element 'rc': ReferenceChangeP sets E_tot_ref, dE_ref; "
                     "only one of them may be given") != std::string::npos);
    REQUIRE(all.find("element 'rc': ReferenceChangeP sets time_ref, dtime_ref; "
                     "only one of them may be given") != std::string::npos);
    free_lat(lat);
}

TEST_CASE("Bookkeeper places the elements after a Patch", "[bookkeeper][floor]") {
    // patch.md: the offsets move the exit frame, and y_rot turns it toward +x
    // (a positive theta, the opposite sense to a positive bend angle).
    struct lattices lat = expand_PALS_string(
        line_around("          - p1:\n"
                    "              kind: Patch\n"
                    "              PatchP:\n"
                    "                x_offset: 0.1\n"
                    "                z_offset: 0.3\n"
                    "          - p2:\n"
                    "              kind: Patch\n"
                    "              PatchP:\n"
                    "                y_rot: 0.25\n")
            .c_str(),
        nullptr);
    INFO(problem_text(lat));
    YAMLTreeHandle t = lat.full_expanded;
    REQUIRE(close(param_num(t, "p2", "FloorP", "x"), 0.1));
    REQUIRE(close(param_num(t, "p2", "FloorP", "z"), 1.3));
    REQUIRE(close(param_num(t, "p2", "FloorP", "theta"), 0.0));
    REQUIRE(close(param_num(t, "d2", "FloorP", "theta"), 0.25));
    REQUIRE(close(param_num(t, "end", "FloorP", "x"), 0.1 + std::sin(0.25)));
    REQUIRE(close(param_num(t, "end", "FloorP", "z"), 1.3 + std::cos(0.25)));
    free_lat(lat);
}

TEST_CASE("Bookkeeper derives the bend length from its chord or rectangle length",
          "[bookkeeper][bend]") {
    // bend.md: L_chord = 2 rho sin(angle/2) and L_rectangle = rho sin(angle),
    // so length = L_chord / sinc(angle/2) = L_rectangle / sinc(angle).
    const double angle = 0.2;
    struct Case {
        const char* given;
        double length;
    };
    const Case cases[] = {
        {"                L_chord: 1.0\n", 1.0 / (std::sin(angle / 2) / (angle / 2))},
        {"                L_rectangle: 1.0\n", 1.0 / (std::sin(angle) / angle)},
    };
    for (const Case& c : cases) {
        INFO(c.given);
        struct lattices lat = expand_PALS_string(
            line_around(std::string("          - b1:\n"
                                    "              kind: Bend\n"
                                    "              BendP:\n"
                                    "                angle_ref: 0.2\n") +
                        c.given)
                .c_str(),
            nullptr);
        INFO(problem_text(lat));
        REQUIRE(lat.problems.count == 0);
        REQUIRE(close(param_num(lat.full_expanded, "b1", nullptr, "length"),
                      c.length));
        free_lat(lat);
    }
}
