#include "test_helpers.h"

// When things are evaluated and copied during lattice expansion
// (lattice-construction.md, s:lattice.expand): immediate expressions at their
// place in the `facility` list, `expr(...)` delayed ones after
// `post_expansion`; inheritance at definition time, and line items that only
// use an element at branch expansion (lattice-elements.md, beamlines.md). Also the rules on where constants and
// variables live and what they may be defined with
// (parameter-values-and-controllers.md, s:constants, s:expressions, s:set).

namespace {

std::vector<std::string> messages(const struct lattices& lat) {
    std::vector<std::string> out;
    for (size_t i = 0; i < lat.problems.count; ++i)
        out.emplace_back(lat.problems.items[i].message);
    return out;
}

std::string joined(const struct lattices& lat) {
    std::string s;
    for (const std::string& m : messages(lat)) s += m + "; ";
    return s;
}

bool any_contains(const struct lattices& lat, const char* needle) {
    for (const std::string& m : messages(lat))
        if (m.find(needle) != std::string::npos) return true;
    return false;
}

void free_all(struct lattices& lat) {
    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

// `defs` then a one-branch lattice whose line is `begin` followed by `line`,
// then `post` as the post_expansion list (if any).
std::string file(const std::string& defs, const std::string& line,
                 const std::string& post = "") {
    return "PALS:\n"
           "  facility:\n" +
           defs +
           "    - ln:\n"
           "        kind: BeamLine\n"
           "        line:\n"
           "          - begin:\n"
           "              kind: BeginningEle\n"
           "              ReferenceP:\n"
           "                species_ref: electron\n"
           "                E_tot_ref: 1.0e9\n" +
           line +
           "    - lat1:\n"
           "        kind: Lattice\n"
           "        branches:\n"
           "          - ln\n"
           "    - use: lat1\n" +
           (post.empty() ? "" : "  post_expansion:\n" + post);
}

// `group.param` (or a top-level `param` when `group` is null) of the first
// element keyed `ele` in `t`.
double param(YAMLTreeHandle t, const char* ele, const char* group,
             const char* p) {
    YAMLNodeId e = find_by_key(t, ele);
    REQUIRE(e != YAML_NULL_ID);
    YAMLNodeId g = group ? get_child_by_key(t, e, group) : e;
    REQUIRE(g != YAML_NULL_ID);
    YAMLNodeId v = get_child_by_key(t, g, p);
    REQUIRE(v != YAML_NULL_ID);
    return num_val(t, v);
}

}  // namespace

TEST_CASE("an immediate expression keeps its value at its place in facility",
          "[expr][lattices][set]") {
    // q2's Kn1 is evaluated when q2 is reached, before the set changes q1, so
    // it keeps 2 * 0.5; q3's delayed Kn1 follows the set.
    const std::string doc = file(
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: 0.5\n"
        "    - q2:\n"
        "        kind: Quadrupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: 2 * q1>MagneticMultipoleP.Kn1\n"
        "    - q3:\n"
        "        kind: Quadrupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: expr(2 * q1>MagneticMultipoleP.Kn1)\n"
        "    - set:\n"
        "        parameter: q1>MagneticMultipoleP.Kn1\n"
        "        value: 0.7\n",
        "          - q1\n"
        "          - q2\n"
        "          - q3\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    REQUIRE(close(param(lat.full_expanded, "q1", "MagneticMultipoleP", "Kn1"), 0.7));
    REQUIRE(close(param(lat.full_expanded, "q2", "MagneticMultipoleP", "Kn1"), 1.0));
    REQUIRE(close(param(lat.full_expanded, "q3", "MagneticMultipoleP", "Kn1"), 1.4));
    free_all(lat);
}

TEST_CASE("delayed expressions are evaluated again after post_expansion",
          "[expr][lattices][set]") {
    // A post_expansion set on q1 reaches q3's delayed Kn1 but not q2's
    // immediate one.
    const std::string doc = file(
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: 0.5\n"
        "    - q2:\n"
        "        kind: Quadrupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: 2 * q1>MagneticMultipoleP.Kn1\n"
        "    - q3:\n"
        "        kind: Quadrupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: expr(2 * q1>MagneticMultipoleP.Kn1)\n",
        "          - q1\n"
        "          - q2\n"
        "          - q3\n",
        "    - set:\n"
        "        parameter: q1>MagneticMultipoleP.Kn1\n"
        "        value: 0.7\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    REQUIRE(close(param(lat.full_expanded, "q2", "MagneticMultipoleP", "Kn1"), 1.0));
    REQUIRE(close(param(lat.full_expanded, "q3", "MagneticMultipoleP", "Kn1"), 1.4));
    free_all(lat);
}

TEST_CASE("a delayed set value is stored as an expression",
          "[expr][lattices][set]") {
    // The facility set writes a delayed value into q2; the post_expansion set
    // on q1 then reaches it.
    const std::string doc = file(
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: 0.5\n"
        "    - q2:\n"
        "        kind: Quadrupole\n"
        "    - set:\n"
        "        parameter: q2>MagneticMultipoleP.Kn1\n"
        "        value: expr(3 * q1>MagneticMultipoleP.Kn1)\n",
        "          - q1\n"
        "          - q2\n",
        "    - set:\n"
        "        parameter: q1>MagneticMultipoleP.Kn1\n"
        "        value: 0.1\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    REQUIRE(close(param(lat.full_expanded, "q2", "MagneticMultipoleP", "Kn1"), 0.3));
    free_all(lat);
}

TEST_CASE("a self-referential delayed set value is reported",
          "[expr][lattices][set][problems]") {
    const std::string doc = file(
        "    - b1:\n"
        "        kind: Bend\n"
        "        BendP:\n"
        "          e1: 0.1\n"
        "    - set:\n"
        "        parameter: b1>BendP.e1\n"
        "        value: expr(2*PARAMETER)\n",
        "          - b1\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(any_contains(lat, "a delayed value may not use PARAMETER"));
    REQUIRE(close(param(lat.full_expanded, "b1", "BendP", "e1"), 0.1));
    free_all(lat);
}

TEST_CASE("inheritance copies the values an element has when it is defined",
          "[lattices][inherit]") {
    // The example of lattice-elements.md: a set of an S2 parameter after S3
    // inherits from S2 does not affect S3.
    const std::string doc = file(
        "    - S2:\n"
        "        kind: Sextupole\n"
        "        length: 0.45\n"
        "        MagneticMultipoleP:\n"
        "          Kn2L: 0.56\n"
        "          tilt2: 0.12\n"
        "    - S3:\n"
        "        inherit: S2\n"
        "        length: 0.55\n"
        "    - set:\n"
        "        parameter: S2>MagneticMultipoleP.Kn2L\n"
        "        value: 0.1\n",
        "          - S2\n"
        "          - S3\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    REQUIRE(close(param(lat.full_expanded, "S2", "MagneticMultipoleP", "Kn2L"), 0.1));
    REQUIRE(close(param(lat.full_expanded, "S3", "MagneticMultipoleP", "Kn2L"), 0.56));
    REQUIRE(close(param(lat.full_expanded, "S3", nullptr, "length"), 0.55));
    free_all(lat);
}

TEST_CASE("an element inherited from must be defined before",
          "[lattices][inherit][problems]") {
    const std::string doc = file(
        "    - S3:\n"
        "        inherit: S2\n"
        "    - S2:\n"
        "        kind: Sextupole\n",
        "          - S2\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(any_contains(lat, "'S3': inherit: 'S2' must be defined before it"));
    free_all(lat);
}

TEST_CASE("a parameter group inherits from another element's group",
          "[lattices][inherit]") {
    // lattice-element-parameters.md, s:inherit.params: `inherit: q1>ApertureP`.
    // The group's own setting wins.
    const std::string doc = file(
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        ApertureP:\n"
        "          x_max: 0.03\n"
        "          y_max: 0.04\n"
        "    - q2:\n"
        "        kind: Quadrupole\n"
        "        ApertureP:\n"
        "          inherit: q1>ApertureP\n"
        "          y_max: 0.05\n",
        "          - q2\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(close(param(lat.full_expanded, "q2", "ApertureP", "x_max"), 0.03));
    REQUIRE(close(param(lat.full_expanded, "q2", "ApertureP", "y_max"), 0.05));
    YAMLNodeId q2 = find_by_key(lat.full_expanded, "q2");
    REQUIRE(get_child_by_key(lat.full_expanded,
                             get_child_by_key(lat.full_expanded, q2, "ApertureP"),
                             "inherit") == YAML_NULL_ID);
    free_all(lat);
}

// The element of line item `i` (counting `begin` as 0) of branch `ln`.
static YAMLNodeId line_item(YAMLTreeHandle t, size_t i) {
    YAMLNodeId ln = find_by_key(t, "ln");
    REQUIRE(ln != YAML_NULL_ID);
    YAMLNodeId wrapper =
        get_child_by_index(t, get_child_by_key(t, ln, "line"), i);
    REQUIRE(wrapper != YAML_NULL_ID);
    return get_child_by_index(t, wrapper, 0);
}

static double item_param(YAMLTreeHandle t, size_t i, const char* group,
                  const char* p) {
    YAMLNodeId e = line_item(t, i);
    YAMLNodeId g = group ? get_child_by_key(t, e, group) : e;
    REQUIRE(g != YAML_NULL_ID);
    YAMLNodeId v = get_child_by_key(t, g, p);
    REQUIRE(v != YAML_NULL_ID);
    return num_val(t, v);
}

TEST_CASE("a keyed line item is an instance of the element it names",
          "[lattices][line]") {
    // beamlines.md: `- thingB: {length: 0.45}` uses thingB defined elsewhere,
    // with a value of its own. A group the instance sets one parameter of
    // keeps the definition's others. A line item that only uses an element
    // gets the element's parameters at branch expansion (names.md,
    // lattice-elements.md), so a set on thingB after the line is defined
    // reaches it, except where it has a value of its own.
    const std::string doc = file(
        "    - thingB:\n"
        "        kind: Sextupole\n"
        "        length: 0.73\n"
        "        MagneticMultipoleP:\n"
        "          Kn2L: 0.5\n"
        "          tilt2: 0.1\n",
        "          - thingB:\n"
        "              length: 0.45\n"
        "              MagneticMultipoleP:\n"
        "                Kn2L: 0.6\n"
        "          - thingB\n");
    // Put the sets after the line definition.
    std::string with_set = doc;
    with_set.insert(with_set.find("    - lat1:"),
                    "    - sets:\n"
                    "        - thingB>length: 0.8\n"
                    "        - thingB>MagneticMultipoleP.tilt2: 0.3\n");

    struct lattices lat = expand_PALS_string(with_set.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    YAMLNodeId first = line_item(lat.full_expanded, 1);
    REQUIRE(key_eq(lat.full_expanded, first, "thingB"));
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, first, "kind"), "Sextupole"));
    REQUIRE(close(item_param(lat.full_expanded, 1, nullptr, "length"), 0.45));
    REQUIRE(close(item_param(lat.full_expanded, 1, "MagneticMultipoleP", "Kn2L"),
                  0.6));
    REQUIRE(close(item_param(lat.full_expanded, 1, "MagneticMultipoleP", "tilt2"),
                  0.3));
    // The bare reference is the definition, set and all: placed after the
    // 0.45 m instance, it starts at s = 0.45 and is 0.8 m long.
    YAMLNodeId end = find_by_key(lat.full_expanded, "branch_end");
    REQUIRE(close(num_val(lat.full_expanded,
                          get_child_by_key(lat.full_expanded, end, "s_position")),
                  1.25));
    free_all(lat);
}

TEST_CASE("a keyed line item may use an element defined after the line",
          "[lattices][line]") {
    // The item only uses Q9; it gets Q9's parameters at branch expansion, by
    // which time the whole facility list has been walked.
    const std::string doc = file(
        "",
        "          - Q9:\n"
        "              length: 0.2\n");
    std::string with_def = doc;
    with_def.insert(with_def.find("    - lat1:"),
                    "    - Q9:\n"
                    "        kind: Quadrupole\n"
                    "        length: 0.5\n"
                    "        MagneticMultipoleP:\n"
                    "          Kn1: 0.4\n");

    struct lattices lat = expand_PALS_string(with_def.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded,
                                    line_item(lat.full_expanded, 1), "kind"),
                   "Quadrupole"));
    REQUIRE(close(item_param(lat.full_expanded, 1, nullptr, "length"), 0.2));
    REQUIRE(close(item_param(lat.full_expanded, 1, "MagneticMultipoleP", "Kn1"),
                  0.4));
    free_all(lat);
}

TEST_CASE("before branch expansion a name matches elements defined in a line",
          "[lattices][line][sets]") {
    // The example of names.md (s:names): an unqualified name matches the
    // construct that defines the element -- under `facility` or in place in a
    // line -- but not a line item that only uses it. Q2, defined in place by
    // inheriting from Q1, inherits immediately, before the sets.
    const std::string doc = file(
        "    - Q1:\n"
        "        kind: Quadrupole\n"
        "        length: 0.5\n"
        "        MagneticMultipoleP:\n"
        "          Bs1: 0.2\n",
        "          - Q1\n"
        "          - Q1:\n"
        "              MagneticMultipoleP:\n"
        "                Bs1: 0.7\n"
        "          - Q2:\n"
        "              inherit: Q1\n"
        "          - S2:\n"
        "              kind: Sextupole\n"
        "              length: 0.1\n"
        "          - S2\n");
    std::string with_sets = doc;
    with_sets.insert(with_sets.find("    - lat1:"),
                     "    - sets:\n"
                     "        - Q1>MagneticMultipoleP.Bs1: 0.45\n"
                     "        - S2>length: 0.3\n");

    struct lattices lat = expand_PALS_string(with_sets.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    REQUIRE(key_eq(lat.full_expanded, line_item(lat.full_expanded, 1), "Q1"));
    REQUIRE(close(item_param(lat.full_expanded, 1, "MagneticMultipoleP", "Bs1"),
                  0.45));
    REQUIRE(close(item_param(lat.full_expanded, 2, "MagneticMultipoleP", "Bs1"),
                  0.7));
    REQUIRE(key_eq(lat.full_expanded, line_item(lat.full_expanded, 3), "Q2"));
    REQUIRE(close(item_param(lat.full_expanded, 3, "MagneticMultipoleP", "Bs1"),
                  0.2));
    REQUIRE(key_eq(lat.full_expanded, line_item(lat.full_expanded, 4), "S2"));
    REQUIRE(close(item_param(lat.full_expanded, 4, nullptr, "length"), 0.3));
    REQUIRE(close(item_param(lat.full_expanded, 5, nullptr, "length"), 0.3));
    free_all(lat);
}

TEST_CASE("UnionEle elements is a list of contained elements",
          "[lattices][check]") {
    // lattice-element-kinds.md, s:unionele.
    const std::string doc = file(
        "    - Sa:\n"
        "        kind: Solenoid\n"
        "        length: 1.3\n"
        "        SolenoidP:\n"
        "          Ksol: 0.2\n"
        "    - Ra:\n"
        "        kind: RFCavity\n"
        "    - MMM:\n"
        "        kind: UnionEle\n"
        "        length: 2.1\n"
        "        elements:\n"
        "          - Sa:\n"
        "              BodyShiftP:\n"
        "                x_offset: 0.03\n"
        "          - Ra\n",
        "          - MMM\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    INFO(joined(lat));
    REQUIRE(lat.problems.count == 0);
    YAMLNodeId mmm = find_by_key(lat.full_expanded, "MMM");
    YAMLNodeId els = get_child_by_key(lat.full_expanded, mmm, "elements");
    REQUIRE(get_size(lat.full_expanded, els) == 2);
    YAMLNodeId sa = get_child_by_key(lat.full_expanded,
                                     get_child_by_index(lat.full_expanded, els, 0),
                                     "Sa");
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, sa, "kind"), "Solenoid"));
    free_all(lat);

    std::string as_dict = doc;
    as_dict.replace(as_dict.find("          - Sa:\n"), 16, "          Sa:\n");
    as_dict.replace(as_dict.find("          - Ra\n"), 15, "          Ra: {}\n");
    lat = expand_PALS_string(as_dict.c_str(), nullptr);
    REQUIRE(any_contains(lat, "'elements' must be a list"));
    free_all(lat);
}

TEST_CASE("constants and variables are lists under facility or post_expansion",
          "[expr][lattices][problems]") {
    struct lattices lat = expand_PALS_string(
        file("    - constants:\n"
             "        a_const: 0.3\n"
             "    - c1:\n"
             "        kind: constant\n"
             "        value: expr(0.2)\n"
             "    - variables:\n"
             "        - v1: expr(0.4)\n",
             "")
            .c_str(),
        nullptr);
    REQUIRE(any_contains(lat, "`constants` must be a list of `name: value` "
                              "entries"));
    REQUIRE(any_contains(lat, "constant 'c1' is defined with a delayed "
                              "expression"));
    // A variable may be delayed.
    REQUIRE(!any_contains(lat, "'v1' is defined with a delayed"));
    free_all(lat);

    lat = expand_PALS_string(
        "PALS:\n"
        "  constants:\n"
        "    - a: 1\n",
        nullptr);
    REQUIRE(any_contains(lat, "`constants` must be defined directly under the "
                              "`facility` or `post_expansion` list"));
    free_all(lat);
}

TEST_CASE("facility may not reference what post_expansion defines",
          "[expr][lattices][problems]") {
    struct lattices lat = expand_PALS_string(
        file("    - d1:\n"
             "        kind: Drift\n"
             "        length: 2 * late_var\n",
             "          - d1\n",
             "    - variables:\n"
             "        - late_var: 0.5\n")
            .c_str(),
        nullptr);
    REQUIRE(any_contains(lat, "'late_var' is defined in `post_expansion` and "
                              "may not be referenced from `facility`"));
    free_all(lat);
}

TEST_CASE("expr(...) must enclose the whole expression", "[expr]") {
    bool ok = true;
    evaluate_pals_expression("2 * expr(1 + 2)", &ok);
    REQUIRE(!ok);
    REQUIRE(evaluate_pals_expression("expr(1 + 2)", &ok) == 3.0);
    REQUIRE(ok);
    evaluate_pals_expression("expr(1) + expr(2)", &ok);
    REQUIRE(!ok);
}

TEST_CASE("degrees and amu are built-in constants", "[expr]") {
    bool ok = false;
    REQUIRE(close(evaluate_pals_expression("15*degrees", &ok),
                  15 * 3.14159265358979323846 / 180));
    REQUIRE(ok);
    REQUIRE(close_rel(evaluate_pals_expression("208*amu", &ok),
                      208 * 9.3149410372e8));
    REQUIRE(ok);
}
