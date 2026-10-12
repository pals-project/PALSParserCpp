#include "test_helpers.h"

// `set` commands: parameter-values-and-controllers.md (s:set) for the syntax,
// and top-level.md (s:facility) for where a set acts depending on whether it
// sits in `facility` or `post_expansion`.

namespace {

std::vector<std::string> problem_messages(const struct lattices& lat) {
    std::vector<std::string> msgs;
    for (size_t i = 0; i < lat.problems.count; ++i)
        msgs.emplace_back(lat.problems.items[i].message);
    return msgs;
}

std::string joined(const struct lattices& lat) {
    std::string s;
    for (const std::string& m : problem_messages(lat)) s += m + "; ";
    return s;
}

bool any_contains(const std::vector<std::string>& msgs, const char* needle) {
    for (const std::string& m : msgs)
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

// A file whose `facility` list is exactly `body` (10-space indented entries),
// followed by a lattice built from the beamline named `line_name` and a `use`,
// and then a `post_expansion` list holding `post` when that is not empty.
std::string facility_file(const std::string& body, const char* line_name,
                          const std::string& post = "") {
    return "PALS:\n"
           "  facility:\n" +
           body + "    - lat1:\n"
                  "        kind: Lattice\n"
                  "        branches:\n"
                  "          - " +
           line_name +
           "\n"
           "    - use: \"lat1\"\n" +
           (post.empty() ? "" : "  post_expansion:\n" + post);
}

// The value of `group.param` on the `index`-th element named `ele` in the
// expanded lattice (index 0 being the first).
double nth_param(YAMLTreeHandle t, const char* ele, size_t index,
                 const char* group, const char* param) {
    std::vector<YAMLNodeId> hits;
    std::vector<YAMLNodeId> stack{get_root(t)};
    while (!stack.empty()) {
        YAMLNodeId n = stack.back();
        stack.pop_back();
        char* k = get_node_key(t, n);
        if (k && std::string(k) == ele) hits.push_back(n);
        yaml_free_string(k);
        // Push children in reverse so the walk visits them in document order.
        size_t sz = get_size(t, n);
        for (size_t i = sz; i-- > 0;) stack.push_back(get_child_by_index(t, n, i));
    }
    REQUIRE(hits.size() > index);
    YAMLNodeId e = hits[index];
    YAMLNodeId g = group ? get_child_by_key(t, e, group) : e;
    REQUIRE(g != YAML_NULL_ID);
    return num_val(t, get_child_by_key(t, g, param));
}

double one_param(YAMLTreeHandle t, const char* ele, const char* group,
                 const char* param) {
    return nth_param(t, ele, 0, group, param);
}

// Whether the first element named `ele` carries `group.param` at all.
bool has_param(YAMLTreeHandle t, const char* ele, const char* group,
               const char* param) {
    YAMLNodeId e = find_by_key(t, ele);
    REQUIRE(e != YAML_NULL_ID);
    YAMLNodeId g = get_child_by_key(t, e, group);
    if (g == YAML_NULL_ID) return false;
    return get_child_by_key(t, g, param) != YAML_NULL_ID;
}

// A beamline holding one BeginningEle plus whatever `body` adds.
const char* BEGIN_ENTRY =
    "          - begin:\n"
    "              kind: BeginningEle\n"
    "              ReferenceP:\n"
    "                species_ref: \"electron\"\n"
    "                E_tot_ref: 1.0e9\n";

}  // namespace

TEST_CASE("a set writes every element its pattern matches",
          "[expr][lattices][set]") {
    // s:set: `PARAMETER` is the current value of the parameter being written
    // and `SELF` reaches the rest of the element.
    const std::string doc =
        facility_file(std::string(
                      "    - B1a:\n"
                      "        kind: Bend\n"
                      "        length: 1.0\n"
                      "        BendP:\n"
                      "          e1: 0.1\n"
                      "          g_ref: 0.02\n"
                      "    - B1b:\n"
                      "        kind: Bend\n"
                      "        length: 1.0\n"
                      "        BendP:\n"
                      "          e1: 0.3\n"
                      "          g_ref: 0.02\n"
                      "    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n") +
                      BEGIN_ENTRY +
                      "          - B1a\n"
                      "          - B1b\n"
                      "          - Q1\n"
                      "    - set:\n"
                      "        parameter: B1.*>BendP.e1\n"
                      "        value: 2*PARAMETER + atan(SELF.BendP.g_ref)\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");

    REQUIRE(close(one_param(lat.full_expanded, "B1a", "BendP", "e1"),
                  2 * 0.1 + std::atan(0.02)));
    REQUIRE(close(one_param(lat.full_expanded, "B1b", "BendP", "e1"),
                  2 * 0.3 + std::atan(0.02)));

    free_all(lat);
}

TEST_CASE("a set only reaches elements defined before it",
          "[expr][lattices][set]") {
    // Q2 is defined after the set, so the set does not touch it.
    const std::string doc =
        facility_file("    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "        MagneticMultipoleP:\n"
                      "          Kn1L: 0.1\n"
                      "    - set:\n"
                      "        parameter: Q.*>MagneticMultipoleP.Kn1L\n"
                      "        value: PARAMETER + 0.02\n"
                      "    - Q2:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "        MagneticMultipoleP:\n"
                      "          Kn1L: 0.1\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - Q1\n"
                      "          - Q2\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");

    REQUIRE(close(one_param(lat.full_expanded, "Q1", "MagneticMultipoleP", "Kn1L"),
                  0.12));
    REQUIRE(close(one_param(lat.full_expanded, "Q2", "MagneticMultipoleP", "Kn1L"),
                  0.1));

    free_all(lat);
}

namespace {

// The example of names.md (s:name.matching), as a whole file: `q1` defined as a
// facility entry, a second `q1` written inside `myline`, and a branch
// inheriting myline so expansion makes a third. `body` is the facility entries
// that follow, and `post` the post_expansion list, which is where each test
// puts its sets.
std::string facility_qualifier_file(const std::string& body,
                                    const std::string& post = "") {
    return "PALS:\n"
           "  facility:\n"
           "    - q1:\n"
           "        kind: Quadrupole\n"
           "        length: 0.1\n"
           "    - myline:\n"
           "        kind: BeamLine\n"
           "        line:\n" +
           std::string(BEGIN_ENTRY) +
           "          - q1:\n"
           "              kind: Quadrupole\n"
           "              length: 0.9\n"
           "    - mylat:\n"
           "        kind: Lattice\n"
           "        branches:\n"
           "          - mybranch:\n"
           "              inherit: myline\n"
           "    - use: \"mylat\"\n" +
           body + (post.empty() ? "" : "  post_expansion:\n" + post);
}

}  // namespace

TEST_CASE("a set on a bare line reference gives it values of its own",
          "[expr][lattices][set]") {
    // `- q1` and `- q1: {length: 0.6}` are the same line entry, the second with
    // room for values the definition does not give it. A set naming the first
    // through its beamline writes it, so the two files expand alike.
    const char* DEF_AND_LINE =
        "PALS:\n"
        "  facility:\n"
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        length: 0.1\n"
        "    - myline:\n"
        "        kind: BeamLine\n"
        "        line:\n";
    const char* TAIL =
        "    - mylat:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - mybranch:\n"
        "              inherit: myline\n"
        "    - use: \"mylat\"\n";

    const std::string with_set = DEF_AND_LINE + std::string(BEGIN_ENTRY) +
                                 "          - q1\n" + TAIL +
                                 "    - sets:\n"
                                 "        - myline>>q1>length: 0.6\n";
    const std::string written_out = DEF_AND_LINE + std::string(BEGIN_ENTRY) +
                                    "          - q1:\n"
                                    "              length: 0.6\n" +
                                    TAIL;

    struct lattices a = expand_PALS_string(with_set.c_str(), nullptr);
    struct lattices b = expand_PALS_string(written_out.c_str(), nullptr);
    REQUIRE(joined(a) == "");
    REQUIRE(joined(b) == "");

    REQUIRE(close(one_param(a.full_expanded, "q1", nullptr, "length"), 0.6));
    REQUIRE(close(one_param(b.full_expanded, "q1", nullptr, "length"), 0.6));

    // The definition the reference points at is not what was named, so it keeps
    // the length it was given.
    struct param_value v = get_parameter_value(a.adjunct, "q1>length");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(close(v.number, 0.1));

    free_all(a);
    free_all(b);
}

TEST_CASE("an unqualified set targets the definition, not the references to it",
          "[expr][lattices][set]") {
    // names.md: before expansion an unqualified name reaches the q1 defined
    // under facility, and every use of it follows along. If the bare `- q1` in
    // the line were written too, its 0.6 would override the definition's 0.7 in
    // the expanded lattice.
    const std::string doc =
        "PALS:\n"
        "  facility:\n"
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        length: 0.1\n"
        "    - myline:\n"
        "        kind: BeamLine\n"
        "        line:\n" +
        std::string(BEGIN_ENTRY) +
        "          - q1\n"
        "    - mylat:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - mybranch:\n"
        "              inherit: myline\n"
        "    - use: \"mylat\"\n"
        "    - sets:\n"
        "        - q1>length: 0.7\n";

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    REQUIRE(close(one_param(lat.full_expanded, "q1", nullptr, "length"), 0.7));

    free_all(lat);
}

TEST_CASE("an unqualified set writes the facility definition only",
          "[expr][lattices][set]") {
    // names.md: before expansion `q1` names the definition standing under
    // facility, not the q1 written inside myline.
    const std::string doc =
        facility_qualifier_file("    - sets:\n"
                                "        - q1>length: 0.4\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");

    struct param_value v = get_parameter_value(lat.adjunct, "q1>length");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(close(v.number, 0.4));

    // The q1 inside myline, and so the copy expansion made from it, is untouched.
    REQUIRE(close(one_param(lat.full_expanded, "q1", nullptr, "length"), 0.9));

    free_all(lat);
}

TEST_CASE("a post_expansion set writes the expanded lattice, not the definitions",
          "[expr][lattices][set]") {
    const std::string doc = facility_qualifier_file(
        "", "    - sets:\n"
            "        - q1>length: 0.5\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");

    REQUIRE(close(one_param(lat.full_expanded, "q1", nullptr, "length"), 0.5));
    struct param_value v = get_parameter_value(lat.adjunct, "q1>length");
    REQUIRE(v.kind == PARAM_VALUE_NUMBER);
    REQUIRE(close(v.number, 0.1));

    free_all(lat);
}

TEST_CASE("a set creates a parameter that defaults to zero",
          "[expr][lattices][set]") {
    // s:lattice.expand: an unwritten parameter of a known element reads as zero,
    // so `PARAMETER + 0.02` on an element with no MagneticMultipoleP is 0.02.
    const std::string doc =
        facility_file("    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "    - set:\n"
                      "        parameter: Q1>MagneticMultipoleP.Kn0\n"
                      "        value: PARAMETER + 0.02\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) + "          - Q1\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    REQUIRE(close(one_param(lat.full_expanded, "Q1", "MagneticMultipoleP", "Kn0"),
                  0.02));

    free_all(lat);
}

TEST_CASE("reading a not-yet-derived parameter in a set is an error",
          "[expr][lattices][set][problems]") {
    // The second example of s:lattice.expand: Q1's Bs1 will be derived from its
    // Ks1 and the reference momentum, which is not known when the set runs, so
    // reading Bs1 here is an error rather than reading zero.
    const std::string doc =
        facility_file("    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "        MagneticMultipoleP:\n"
                      "          Ks1: 0.34\n"
                      "    - Q2:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "    - set:\n"
                      "        parameter: Q2>MagneticMultipoleP.Bs1\n"
                      "        value: Q1>MagneticMultipoleP.Bs1\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - Q1\n"
                      "          - Q2\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(any_contains(problem_messages(lat),
                         "'Q1>MagneticMultipoleP.Bs1' has no value yet"));

    free_all(lat);
}

TEST_CASE("an unwritten parameter with no written family member reads as zero",
          "[expr][lattices][set]") {
    // The same shape as the error case, but with Ks1 left unset: nothing in the
    // family is written, so Bs1 is simply zero and there is no error.
    const std::string doc =
        facility_file("    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "    - Q2:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "    - set:\n"
                      "        parameter: Q2>MagneticMultipoleP.Kn1L\n"
                      "        value: Q1>MagneticMultipoleP.Bs1 + 0.5\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - Q1\n"
                      "          - Q2\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    REQUIRE(close(one_param(lat.full_expanded, "Q2", "MagneticMultipoleP", "Kn1L"),
                  0.5));

    free_all(lat);
}

TEST_CASE("the compact sets form writes each pair", "[expr][lattices][set]") {
    const std::string doc =
        facility_file("    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "    - D1:\n"
                      "        kind: Drift\n"
                      "        length: 1.0\n"
                      "    - sets:\n"
                      "        - Q1>MagneticMultipoleP.Kn1L: 0.25\n"
                      "        - D1>length: 2 * 1.5\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - Q1\n"
                      "          - D1\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    REQUIRE(close(one_param(lat.full_expanded, "Q1", "MagneticMultipoleP", "Kn1L"),
                  0.25));
    REQUIRE(close(one_param(lat.full_expanded, "D1", nullptr, "length"), 3.0));

    free_all(lat);
}

TEST_CASE("a facility set writes the single definition",
          "[expr][lattices][set]") {
    // top-level.md, s:facility: the repeated element has not been instantiated
    // yet, so the set targets the one definition and all three copies inherit
    // its value.
    const std::string doc =
        facility_file("    - q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "        MagneticMultipoleP:\n"
                      "          Kn1L: 0.375\n"
                      "    - set:\n"
                      "        parameter: q1>MagneticMultipoleP.Kn1L\n"
                      "        value: PARAMETER * 2\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - q1:\n"
                      "              repeat: 3\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    for (size_t i = 0; i < 3; ++i)
        REQUIRE(close(nth_param(lat.full_expanded, "q1", i, "MagneticMultipoleP",
                                "Kn1L"),
                      0.75));

    free_all(lat);
}

TEST_CASE("a post_expansion set reaches each expanded copy",
          "[expr][lattices][set]") {
    // The point of `post_expansion`: with the lattice built, the three copies of
    // `q1` are separate elements and the set writes each of them. The value used
    // here depends on the copy's own s_position, which only exists after
    // expansion, so it also pins down that the bookkeeper has already run.
    const std::string doc =
        facility_file("    - q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "        MagneticMultipoleP:\n"
                      "          Kn1L: 0.375\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - q1:\n"
                      "              repeat: 3\n",
                      "main",
                      "    - set:\n"
                      "        parameter: lat1>>>q1>MagneticMultipoleP.Kn1L\n"
                      "        value: PARAMETER + SELF.s_position\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");

    // The copies sit at s = 0, 0.5 and 1.0.
    for (size_t i = 0; i < 3; ++i)
        REQUIRE(close(nth_param(lat.full_expanded, "q1", i, "MagneticMultipoleP",
                                "Kn1L"),
                      0.375 + 0.5 * static_cast<double>(i)));

    free_all(lat);
}

TEST_CASE("a post-expansion set makes the bookkeeper redo the family",
          "[expr][lattices][set]") {
    // Writing Kn1L after the bookkeeper has already derived Bn1L from the old
    // value must not leave the two inconsistent (nor be reported as such): the
    // stale family members are dropped and the second bookkeeper pass rebuilds
    // them. q2 is written out with the value the set gives q1, so the derived
    // Bn1L must come out the same for both.
    const std::string doc =
        facility_file("    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - q1:\n"
                      "              kind: Quadrupole\n"
                      "              length: 0.5\n"
                      "              MagneticMultipoleP:\n"
                      "                Kn1L: 0.375\n"
                      "          - q2:\n"
                      "              kind: Quadrupole\n"
                      "              length: 0.5\n"
                      "              MagneticMultipoleP:\n"
                      "                Kn1L: 0.75\n",
                      "main",
                      "    - set:\n"
                      "        parameter: q1>MagneticMultipoleP.Kn1L\n"
                      "        value: PARAMETER * 2\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");

    REQUIRE(close(one_param(lat.full_expanded, "q1", "MagneticMultipoleP", "Kn1L"),
                  0.75));
    double b1 = one_param(lat.full_expanded, "q1", "MagneticMultipoleP", "Bn1L");
    double b2 = one_param(lat.full_expanded, "q2", "MagneticMultipoleP", "Bn1L");
    REQUIRE(b1 != 0.0);
    REQUIRE(close_rel(b1, b2));

    free_all(lat);
}

TEST_CASE("a pre-expansion set nullifies the rest of its family",
          "[expr][lattices][set]") {
    // s:set, the interdependent-parameter example verbatim: s1
    // states its sextupole skew component as Ks2, and a set then states the
    // same component as Ks2L. The set nullifies the Ks2 the definition gave, so
    // the two are not left inconsistent -- the component is what the set says,
    // and the bookkeeper derives Ks2 back from it over the element length.
    const std::string doc =
        facility_file("    - s1:\n"
                      "        kind: Sextupole\n"
                      "        length: 0.27\n"
                      "        MagneticMultipoleP:\n"
                      "          Ks2: 0.34\n"
                      "    - set:\n"
                      "        parameter: s1>MagneticMultipoleP.Ks2L\n"
                      "        value: 0.5\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) + "          - s1\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");

    // Only the set's own parameter survives as an input; the nullified Ks2 is
    // no longer something the author asked for.
    REQUIRE(has_param(lat.expanded, "s1", "MagneticMultipoleP", "Ks2L"));
    REQUIRE(!has_param(lat.expanded, "s1", "MagneticMultipoleP", "Ks2"));
    REQUIRE(close(one_param(lat.full_expanded, "s1", "MagneticMultipoleP", "Ks2L"),
                  0.5));
    REQUIRE(close_rel(one_param(lat.full_expanded, "s1", "MagneticMultipoleP",
                                "Ks2"),
                      0.5 / 0.27));

    free_all(lat);
}

TEST_CASE("a post-expansion set that changes a length moves the lattice",
          "[expr][lattices][set]") {
    // "Recalculate floor and reference parameters as needed": lengthening the
    // drift after expansion has to move everything downstream of it.
    const std::string doc =
        facility_file("    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - d1:\n"
                      "              kind: Drift\n"
                      "              length: 1.0\n"
                      "          - m1:\n"
                      "              kind: Marker\n",
                      "main",
                      "    - set:\n"
                      "        parameter: d1>length\n"
                      "        value: 3.5\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    REQUIRE(close(one_param(lat.full_expanded, "m1", nullptr, "s_position"), 3.5));
    REQUIRE(close(one_param(lat.full_expanded, "m1", "FloorP", "z"), 3.5));

    free_all(lat);
}

TEST_CASE("controllers stay authoritative over a post-expansion set",
          "[expr][lattices][set][controller]") {
    // s:lattice.expand applies the ABSOLUTE controllers after the
    // post_expansion list, so a controller-driven parameter keeps the
    // controller's value even when a later set writes it.
    const std::string doc =
        facility_file("    - ps1:\n"
                      "        kind: Controller\n"
                      "        controls:\n"
                      "          - parameter: q1>MagneticMultipoleP.Kn1L\n"
                      "            expression: 0.9\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) +
                      "          - q1:\n"
                      "              kind: Quadrupole\n"
                      "              length: 0.5\n"
                      "              MagneticMultipoleP:\n"
                      "                Kn1L: 0.1\n",
                      "main",
                      "    - set:\n"
                      "        parameter: q1>MagneticMultipoleP.Kn1L\n"
                      "        value: 0.2\n");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    REQUIRE(close(one_param(lat.full_expanded, "q1", "MagneticMultipoleP", "Kn1L"),
                  0.9));

    free_all(lat);
}

TEST_CASE("a set with an error term reports that it is not applied",
          "[expr][lattices][set][problems]") {
    // The standard gives the error magnitude but not its distribution, and this
    // library never invents randomness, so the deterministic value is written
    // and the error is flagged.
    const std::string doc =
        facility_file("    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "        MagneticMultipoleP:\n"
                      "          Kn1L: 0.4\n"
                      "    - set:\n"
                      "        parameter: Q1>MagneticMultipoleP.Kn1L\n"
                      "        value: PARAMETER\n"
                      "        relative_error: 1e-4\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) + "          - Q1\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(any_contains(problem_messages(lat),
                         "absolute_error/relative_error are not applied"));
    REQUIRE(close(one_param(lat.full_expanded, "Q1", "MagneticMultipoleP", "Kn1L"),
                  0.4));

    free_all(lat);
}

TEST_CASE("a set whose target matches nothing is reported",
          "[expr][lattices][set][problems]") {
    const std::string doc =
        facility_file("    - set:\n"
                      "        parameter: nosuch>length\n"
                      "        value: 1.0\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY),
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(any_contains(problem_messages(lat),
                         "set 'nosuch>length': target matches nothing defined "
                         "before it"));

    free_all(lat);
}

TEST_CASE("a set with a random value is deferred", "[expr][lattices][set]") {
    // random()/random_gauss() are deferred everywhere so the expanded tree stays
    // reproducible; a set driven by one leaves its parameter alone.
    const std::string doc =
        facility_file("    - Q1:\n"
                      "        kind: Quadrupole\n"
                      "        length: 0.5\n"
                      "        MagneticMultipoleP:\n"
                      "          Kn1L: 0.375\n"
                      "    - set:\n"
                      "        parameter: Q1>MagneticMultipoleP.Kn1L\n"
                      "        value: PARAMETER * (1 + 1e-4*random_gauss())\n"
                      "    - main:\n"
                      "        kind: BeamLine\n"
                      "        line:\n" +
                      std::string(BEGIN_ENTRY) + "          - Q1\n",
                      "main");

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    REQUIRE(joined(lat) == "");
    REQUIRE(close(one_param(lat.full_expanded, "Q1", "MagneticMultipoleP", "Kn1L"),
                  0.375));

    free_all(lat);
}
