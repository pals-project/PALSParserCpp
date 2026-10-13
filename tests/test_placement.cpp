#include "test_helpers.h"

// ============================================================
// FLOOR PLACEMENT: FloorShift, Fiducial, Girder, ShiftedFloorP
// ============================================================
//
// coordinateset.md for the CoordinateSetP group, lattice-element-kinds.md for
// FloorShift, Fiducial and Girder, shiftedfloor.md, and coordinates.md
// (s:straight.mis, s:bend.mis, s:girder.mis) for the shift geometry.

namespace {

std::string joined(const struct lattices& lat) {
    std::string s;
    for (size_t i = 0; i < lat.problems.count; ++i)
        s += std::string(lat.problems.items[i].message) + "; ";
    return s;
}

bool any_contains(const struct lattices& lat, const char* needle) {
    return joined(lat).find(needle) != std::string::npos;
}

// The `n`-th (from 1) element named `name` anywhere in `t`.
YAMLNodeId element(YAMLTreeHandle t, const char* name, int n = 1) {
    std::string spec = std::string(name) + "#" + std::to_string(n);
    struct name_matches m = match_names(t, spec.c_str());
    REQUIRE(m.count == 1);
    YAMLNodeId e = m.nodes[0];
    free_name_matches(m);
    return e;
}

double param(YAMLTreeHandle t, YAMLNodeId ele, const char* group,
             const char* key) {
    YAMLNodeId g = get_child_by_key(t, ele, group);
    REQUIRE(g != YAML_NULL_ID);
    YAMLNodeId v = get_child_by_key(t, g, key);
    REQUIRE(v != YAML_NULL_ID);
    return num_val(t, v);
}

// Check a placement group (FloorP or ShiftedFloorP) against x, y, z, theta.
void chk_place(YAMLTreeHandle t, YAMLNodeId ele, const char* group, double x,
               double y, double z, double theta = 0.0) {
    INFO(group);
    REQUIRE(param(t, ele, group, "x") == Catch::Approx(x).margin(1e-12));
    REQUIRE(param(t, ele, group, "y") == Catch::Approx(y).margin(1e-12));
    REQUIRE(param(t, ele, group, "z") == Catch::Approx(z).margin(1e-12));
    REQUIRE(param(t, ele, group, "theta") ==
            Catch::Approx(theta).margin(1e-12));
}

// A one-branch lattice whose line is `begin` followed by `line` (entries at
// 10-space indent), with `defs` (4-space facility entries) ahead of it and
// `extra` (2-space PALS keys) after.
std::string doc(const std::string& defs, const std::string& line,
                const std::string& extra = "") {
    return "PALS:\n"
           "  facility:\n" +
           defs +
           "    - main:\n"
           "        kind: BeamLine\n"
           "        line:\n"
           "          - begin:\n"
           "              kind: BeginningEle\n"
           "              ReferenceP:\n"
           "                species_ref: electron\n"
           "                E_tot_ref: 1e9\n" +
           line +
           "    - lat:\n"
           "        kind: Lattice\n"
           "        branches:\n"
           "          - main\n" +
           extra;
}

const char* D1 =
    "          - d1:\n"
    "              kind: Drift\n"
    "              length: 1\n";
const char* D2 =
    "          - d2:\n"
    "              kind: Drift\n"
    "              length: 1\n";
const char* M1 =
    "          - m1:\n"
    "              kind: Marker\n";

}  // namespace

TEST_CASE("a FloorShift places its exit relative to the previous element",
          "[placement]") {
    Expanded x(doc("",
                   std::string(D1) +
                       "          - fs:\n"
                       "              kind: FloorShift\n"
                       "              CoordinateSetP:\n"
                       "                origin_ele: PREVIOUS_ELEMENT\n"
                       "                x_offset: 0.5\n" +
                       D2 + M1)
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();

    // Upstream is untouched; the FloorShift itself starts where d1 ends.
    chk_place(t, element(t, "d1"), "FloorP", 0, 0, 0);
    chk_place(t, element(t, "fs"), "FloorP", 0, 0, 1);
    // Downstream starts from the shifted exit.
    chk_place(t, element(t, "d2"), "FloorP", 0.5, 0, 1);
    chk_place(t, element(t, "m1"), "FloorP", 0.5, 0, 2);
    // Its BodyShiftP places it rather than misaligning it.
    chk_place(t, element(t, "fs"), "ShiftedFloorP", 0, 0, 1);
}

TEST_CASE("a FloorShift defaults to the global origin and takes its BodyShiftP",
          "[placement]") {
    Expanded x(doc("",
                   std::string(D1) +
                       "          - fs:\n"
                       "              kind: FloorShift\n"
                       "              BodyShiftP:\n"
                       "                z_offset: 10\n"
                       "                y_rot: 0.1\n" +
                       D2)
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    chk_place(t, element(t, "d2"), "FloorP", 0, 0, 10, 0.1);
}

TEST_CASE("a FloorShift may be placed from a named element's entrance",
          "[placement]") {
    Expanded x(doc("",
                   std::string(D1) + D2 +
                       "          - fs:\n"
                       "              kind: FloorShift\n"
                       "              CoordinateSetP:\n"
                       "                origin_ele: d2\n"
                       "                origin_ele_ref_pt: ENTRANCE_END\n"
                       "                y_offset: 0.25\n" +
                       M1)
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    chk_place(t, element(t, "m1"), "FloorP", 0, 0.25, 1);
}

TEST_CASE("a FloorShift cannot be placed from an element placed after it",
          "[placement][problems]") {
    Expanded x(doc("",
                   std::string(D1) +
                       "          - fs:\n"
                       "              kind: FloorShift\n"
                       "              CoordinateSetP:\n"
                       "                origin_ele: m1\n" +
                       M1)
                   .c_str());
    REQUIRE(any_contains(x.lat, "is not known before"));
}

TEST_CASE("a Fiducial moves the elements both upstream and downstream",
          "[placement]") {
    Expanded x(doc("",
                   std::string(D1) +
                       "          - fid:\n"
                       "              kind: Fiducial\n"
                       "              CoordinateSetP:\n"
                       "                x_offset: 2\n"
                       "                z_offset: 5\n" +
                       D2 + M1)
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();

    chk_place(t, element(t, "begin"), "FloorP", 2, 0, 4);
    chk_place(t, element(t, "d1"), "FloorP", 2, 0, 4);
    chk_place(t, element(t, "fid"), "FloorP", 2, 0, 5);
    chk_place(t, element(t, "fid"), "ShiftedFloorP", 2, 0, 5);
    chk_place(t, element(t, "d2"), "FloorP", 2, 0, 5);
    chk_place(t, element(t, "m1"), "FloorP", 2, 0, 6);
}

TEST_CASE("a Fiducial rotation turns the whole rigid stretch", "[placement]") {
    const double a = 0.2;
    Expanded x(doc("",
                   std::string(D1) +
                       "          - fid:\n"
                       "              kind: Fiducial\n"
                       "              BodyShiftP:\n"
                       "                y_rot: 0.2\n" +
                       D2)
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    // The Fiducial sits at the origin pointing along theta = a, so d1 starts a
    // metre behind it along that direction.
    chk_place(t, element(t, "fid"), "FloorP", 0, 0, 0, a);
    chk_place(t, element(t, "d1"), "FloorP", -std::sin(a), 0, -std::cos(a), a);
    chk_place(t, element(t, "d2"), "FloorP", 0, 0, 0, a);
}

TEST_CASE("a FloorShift bounds the stretch a Fiducial moves", "[placement]") {
    Expanded x(doc("",
                   std::string(D1) +
                       "          - fs:\n"
                       "              kind: FloorShift\n"
                       "              CoordinateSetP:\n"
                       "                origin_ele: PREVIOUS_ELEMENT\n" +
                       D2 +
                       "          - fid:\n"
                       "              kind: Fiducial\n"
                       "              CoordinateSetP:\n"
                       "                origin_ele: d1\n"
                       "                y_offset: 3\n" +
                       M1)
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    // d1 is upstream of the FloorShift and so not moved by the Fiducial.
    chk_place(t, element(t, "d1"), "FloorP", 0, 0, 0);
    // The Fiducial sits 3 m above d1's exit, and d2 just ahead of it.
    chk_place(t, element(t, "fid"), "FloorP", 0, 3, 1);
    chk_place(t, element(t, "d2"), "FloorP", 0, 3, 0);
    chk_place(t, element(t, "m1"), "FloorP", 0, 3, 1);
}

TEST_CASE("two Fiducials cannot fix the same rigid stretch",
          "[placement][problems]") {
    Expanded x(doc("",
                   "          - f1:\n"
                   "              kind: Fiducial\n" +
                       std::string(D1) +
                       "          - f2:\n"
                       "              kind: Fiducial\n")
                   .c_str());
    REQUIRE(any_contains(x.lat, "both fix the placement"));
}

TEST_CASE("a Fiducial cannot be placed from an element its stretch moves",
          "[placement][problems]") {
    Expanded x(doc("",
                   std::string(D1) +
                       "          - fid:\n"
                       "              kind: Fiducial\n"
                       "              CoordinateSetP:\n"
                       "                origin_ele: PREVIOUS_ELEMENT\n")
                   .c_str());
    REQUIRE(any_contains(x.lat, "PREVIOUS_ELEMENT"));
}

TEST_CASE("ShiftedFloorP equals FloorP when nothing is shifted", "[placement]") {
    Expanded x(doc("",
                   "          - b1:\n"
                   "              kind: Bend\n"
                   "              length: 2\n"
                   "              BendP:\n"
                   "                angle_ref: 0.3\n"
                   "                tilt_ref: 0.4\n" +
                       std::string(M1))
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    for (const char* name : {"begin", "b1", "m1"}) {
        YAMLNodeId e = element(t, name);
        for (const char* k : {"x", "y", "z", "theta", "phi", "psi"})
            REQUIRE(param(t, e, "ShiftedFloorP", k) ==
                    Catch::Approx(param(t, e, "FloorP", k)).margin(1e-12));
    }
    // The branch_end Placeholder holds the final floor and nothing else.
    YAMLNodeId end = element(t, "branch_end");
    REQUIRE(get_child_by_key(t, end, "ShiftedFloorP") == YAML_NULL_ID);
}

TEST_CASE("ShiftedFloorP carries a BodyShiftP about the element centre",
          "[placement]") {
    const double a = 0.01, L = 2.0;
    Expanded x(doc("",
                   std::string(D1) +
                       "          - q1:\n"
                       "              kind: Quadrupole\n"
                       "              length: 2\n"
                       "              BodyShiftP:\n"
                       "                x_offset: 0.001\n"
                       "                y_rot: 0.01\n")
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    YAMLNodeId q = element(t, "q1");
    chk_place(t, q, "FloorP", 0, 0, 1);
    // The centre moves by the offset only; the rotation is about the centre.
    chk_place(t, q, "ShiftedFloorP", 0.001 - L / 2 * std::sin(a), 0,
              1 + L / 2 * (1 - std::cos(a)), a);
}

TEST_CASE("a bend's BodyShiftP is measured in the frame at its chord centre",
          "[placement]") {
    const double ang = 0.4, d = 0.003;
    Expanded x(doc("",
                   "          - b1:\n"
                   "              kind: Bend\n"
                   "              length: 2\n"
                   "              BendP:\n"
                   "                angle_ref: 0.4\n"
                   "              BodyShiftP:\n"
                   "                x_offset: 0.003\n")
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    // The frame at the chord centre is turned by half the bend angle, so its
    // x-axis points along (cos(ang/2), 0, sin(ang/2)); an x_offset slides the
    // whole bend that way without turning it.
    chk_place(t, element(t, "b1"), "ShiftedFloorP", d * std::cos(ang / 2), 0,
              d * std::sin(ang / 2));
}

namespace {

// d1 (1 m), q1 at z = 1..1.5, d2 (1 m), q2 at z = 2.5..3, m1 at z = 3.
const char* GIRDER_LINE =
    "          - d1:\n"
    "              kind: Drift\n"
    "              length: 1\n"
    "          - q1:\n"
    "              kind: Quadrupole\n"
    "              length: 0.5\n"
    "          - d2:\n"
    "              kind: Drift\n"
    "              length: 1\n"
    "          - q2:\n"
    "              kind: Quadrupole\n"
    "              length: 0.5\n"
    "          - m1:\n"
    "              kind: Marker\n";

YAMLNodeId girder(YAMLTreeHandle adjunct, const char* name) {
    YAMLNodeId pals = get_child_by_key(adjunct, get_root(adjunct), "PALS");
    YAMLNodeId g = get_child_by_key(adjunct, pals, "girders");
    REQUIRE(g != YAML_NULL_ID);
    YAMLNodeId e = get_child_by_key(adjunct, g, name);
    REQUIRE(e != YAML_NULL_ID);
    return e;
}

}  // namespace

TEST_CASE("a Girder offset moves every element it supports", "[placement]") {
    Expanded x(doc("", GIRDER_LINE,
                   "  girders:\n"
                   "    g1:\n"
                   "      kind: Girder\n"
                   "      GirderP:\n"
                   "        elements: q1:q2\n"
                   "      CoordinateSetP:\n"
                   "        origin_ele: q1\n"
                   "        origin_ele_ref_pt: ENTRANCE_END\n"
                   "      BodyShiftP:\n"
                   "        x_offset: 0.01\n")
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    chk_place(t, element(t, "d1"), "ShiftedFloorP", 0, 0, 0);
    chk_place(t, element(t, "q1"), "ShiftedFloorP", 0.01, 0, 1);
    chk_place(t, element(t, "d2"), "ShiftedFloorP", 0.01, 0, 1.5);
    chk_place(t, element(t, "q2"), "ShiftedFloorP", 0.01, 0, 2.5);
    chk_place(t, element(t, "m1"), "ShiftedFloorP", 0, 0, 3);

    // The girder's own placement, in the adjunct where it lives.
    YAMLNodeId g = girder(x.lat.adjunct, "g1");
    chk_place(x.lat.adjunct, g, "FloorP", 0, 0, 1);
    chk_place(x.lat.adjunct, g, "ShiftedFloorP", 0.01, 0, 1);
}

TEST_CASE("a Girder rotation turns its elements about the girder origin",
          "[placement]") {
    const double a = 0.02;
    Expanded x(doc("", GIRDER_LINE,
                   "  girders:\n"
                   "    g1:\n"
                   "      kind: Girder\n"
                   "      GirderP:\n"
                   "        elements: q2\n"
                   "      CoordinateSetP:\n"
                   "        origin_ele: q1\n"
                   "        origin_ele_ref_pt: ENTRANCE_END\n"
                   "      BodyShiftP:\n"
                   "        y_rot: 0.02\n")
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    // q2's entrance is 1.5 m downstream of the girder origin at z = 1.
    chk_place(t, element(t, "q2"), "ShiftedFloorP", 1.5 * std::sin(a), 0,
              1 + 1.5 * std::cos(a), a);
    chk_place(t, element(t, "q1"), "ShiftedFloorP", 0, 0, 1);
}

TEST_CASE("a Girder may support another Girder", "[placement]") {
    Expanded x(doc("", GIRDER_LINE,
                   "  girders:\n"
                   "    outer:\n"
                   "      kind: Girder\n"
                   "      GirderP:\n"
                   "        elements: inner, q1\n"
                   "      BodyShiftP:\n"
                   "        x_offset: 0.01\n"
                   "    inner:\n"
                   "      kind: Girder\n"
                   "      GirderP:\n"
                   "        elements: q2\n"
                   "      BodyShiftP:\n"
                   "        x_offset: 0.002\n")
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    chk_place(t, element(t, "q1"), "ShiftedFloorP", 0.01, 0, 1);
    chk_place(t, element(t, "q2"), "ShiftedFloorP", 0.012, 0, 2.5);
    chk_place(x.lat.adjunct, girder(x.lat.adjunct, "inner"), "ShiftedFloorP",
              0.012, 0, 0);
}

TEST_CASE("Girder mistakes are reported", "[placement][problems]") {
    SECTION("an element supported twice") {
        Expanded x(doc("", GIRDER_LINE,
                       "  girders:\n"
                       "    g1:\n"
                       "      kind: Girder\n"
                       "      GirderP:\n"
                       "        elements: q1\n"
                       "    g2:\n"
                       "      kind: Girder\n"
                       "      GirderP:\n"
                       "        elements: q.*\n")
                       .c_str());
        REQUIRE(any_contains(x.lat, "is supported by both"));
    }
    SECTION("a girder placed from the previous element") {
        Expanded x(doc("", GIRDER_LINE,
                       "  girders:\n"
                       "    g1:\n"
                       "      kind: Girder\n"
                       "      GirderP:\n"
                       "        elements: q1\n"
                       "      CoordinateSetP:\n"
                       "        origin_ele: PREVIOUS_ELEMENT\n")
                       .c_str());
        REQUIRE(any_contains(x.lat, "may not be PREVIOUS_ELEMENT"));
    }
    SECTION("a circular chain of support") {
        Expanded x(doc("", GIRDER_LINE,
                       "  girders:\n"
                       "    g1:\n"
                       "      kind: Girder\n"
                       "      GirderP:\n"
                       "        elements: g2\n"
                       "    g2:\n"
                       "      kind: Girder\n"
                       "      GirderP:\n"
                       "        elements: g1\n")
                       .c_str());
        REQUIRE(any_contains(x.lat, "circular chain"));
    }
    SECTION("a girder outside the girders node") {
        Expanded x(doc("    - g1:\n"
                       "        kind: Girder\n"
                       "        GirderP:\n"
                       "          elements: q1\n",
                       GIRDER_LINE)
                       .c_str());
        REQUIRE(any_contains(x.lat, "must be defined under 'PALS: girders'"));
    }
}

TEST_CASE("a Fork places its new branch whether or not it propagates the "
          "reference",
          "[placement][fork]") {
    Expanded x(
        "PALS:\n"
        "  facility:\n"
        "    - dump_begin:\n"
        "        kind: BeginningEle\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 2e9\n"
        "    - dump:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - dump_begin\n"
        "          - dd:\n"
        "              kind: Drift\n"
        "              length: 1\n"
        "    - main:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - begin:\n"
        "              kind: BeginningEle\n"
        "              ReferenceP:\n"
        "                species_ref: electron\n"
        "                E_tot_ref: 1e9\n"
        "          - d1:\n"
        "              kind: Drift\n"
        "              length: 4\n"
        "          - f1:\n"
        "              kind: Fork\n"
        "              ForkP:\n"
        "                to_line: dump\n"
        "                propagate_reference: false\n"
        "    - lat:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main\n");
    REQUIRE(joined(x.lat) == "");
    YAMLTreeHandle t = x.full();
    // The floor comes from the Fork; the reference is the branch's own.
    chk_place(t, element(t, "dump_begin"), "FloorP", 0, 0, 4);
    chk_place(t, element(t, "dd"), "FloorP", 0, 0, 4);
    YAMLNodeId rp = get_child_by_key(t, element(t, "dd"), "ReferenceP");
    REQUIRE(val_eq(t, get_child_by_key(t, rp, "species_ref"), "proton"));
}

TEST_CASE("Placeholder elements do not survive expansion", "[placement]") {
    Expanded x(doc("",
                   std::string(D1) +
                       "          - ph:\n"
                       "              kind: Placeholder\n" +
                       M1)
                   .c_str());
    REQUIRE(joined(x.lat) == "");
    struct name_matches m = match_names(x.full(), "ph");
    REQUIRE(m.count == 0);
    free_name_matches(m);
    // The element after it moves up into its place.
    REQUIRE(num_val(x.full(), get_child_by_key(x.full(), element(x.full(), "m1"),
                                               "element_index")) == 3);
}

TEST_CASE("constants and variables may not be redefined or circular",
          "[expr][problems]") {
    SECTION("the same value twice is fine") {
        Expanded x(doc("    - constants:\n"
                       "        - c1: 2\n"
                       "    - constants:\n"
                       "        - c1: 1 + 1\n",
                       ""));
        REQUIRE(!any_contains(x.lat, "defined more than once"));
    }
    SECTION("a different value is not") {
        Expanded x(doc("    - variables:\n"
                       "        - v1: 37.5\n"
                       "    - variables:\n"
                       "        - v1: 0.13\n",
                       ""));
        REQUIRE(any_contains(x.lat, "'v1' is defined more than once"));
    }
    SECTION("a circular definition") {
        Expanded x(doc("    - constants:\n"
                       "        - a: b + 1\n"
                       "        - b: c * 2\n"
                       "        - c: a\n",
                       ""));
        REQUIRE(any_contains(x.lat, "circular definition"));
    }
}

TEST_CASE("twopi is a built-in constant", "[expr]") {
    bool ok = false;
    REQUIRE(evaluate_pals_expression("twopi / pi", &ok) == Catch::Approx(2.0));
    REQUIRE(ok);
}

TEST_CASE("post_expansion may not hold a Lattice or a use",
          "[check][problems]") {
    Expanded x(doc("", "",
                   "  post_expansion:\n"
                   "    - lat2:\n"
                   "        kind: Lattice\n"
                   "        branches:\n"
                   "          - main\n"
                   "    - use: lat2\n"));
    REQUIRE(any_contains(x.lat, "post_expansion: Lattice 'lat2'"));
    REQUIRE(any_contains(x.lat, "post_expansion: a 'use'"));
}

TEST_CASE("adjustments are checked for shape", "[check][problems]") {
    Expanded good(doc("    - qa:\n"
                      "        kind: Quadrupole\n",
                      "",
                      "  adjustments:\n"
                      "    - optimize:\n"
                      "        - vary:\n"
                      "            parameter: qa>MagneticMultipoleP.Kn1\n"
                      "            weight: 0.1\n"
                      "        - target:\n"
                      "            parameter: fractional_tune_a\n"
                      "            value: 0.37*twopi\n"));
    REQUIRE(!any_contains(good.lat, "adjustments"));

    Expanded bad(doc("", "",
                     "  adjustments:\n"
                     "    - optimize:\n"
                     "        - vary:\n"
                     "            weight: 0.1\n"
                     "        - aim:\n"
                     "            parameter: tune_a\n"));
    REQUIRE(any_contains(bad.lat, "'vary' has no 'parameter'"));
    REQUIRE(any_contains(bad.lat, "unknown optimize entry 'aim'"));
    REQUIRE(any_contains(bad.lat, "needs at least one 'vary' and one 'target'"));
}

TEST_CASE("removed parameters are no longer recognised", "[check][problems]") {
    Expanded x(doc("",
                   "          - q1:\n"
                   "              kind: Quadrupole\n"
                   "              field_master: true\n"
                   "          - f1:\n"
                   "              kind: Fork\n"
                   "              ForkP:\n"
                   "                to_line: main\n"
                   "                new_branch: null\n"
                   "                direction: FORWARDS\n"
                   "          - fs:\n"
                   "              kind: FloorShift\n"
                   "              FloorShiftP:\n"
                   "                x_offset: 1\n"));
    REQUIRE(any_contains(x.lat, "unknown parameter 'field_master'"));
    REQUIRE(any_contains(x.lat, "element 'f1>ForkP': unknown parameter 'direction'"));
    REQUIRE(any_contains(x.lat, "unknown parameter group 'FloorShiftP'"));
}

TEST_CASE("the CoordinateSetP reference point is an enum", "[check][problems]") {
    Expanded x(doc("",
                   "          - fs:\n"
                   "              kind: FloorShift\n"
                   "              CoordinateSetP:\n"
                   "                origin_ele_ref_pt: MIDDLE\n"));
    REQUIRE(any_contains(x.lat, "'origin_ele_ref_pt' is set to 'MIDDLE'"));
}
