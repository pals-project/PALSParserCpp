#include "test_helpers.h"

// ============================================================
// EXPRESSION EVALUATION
// ============================================================

// Evaluate a standalone expression, requiring success, and return its value.
static double eval_ok(const char* expr) {
    bool ok = false;
    double v = evaluate_pals_expression(expr, &ok);
    REQUIRE(ok);
    return v;
}

TEST_CASE("evaluate_pals_expression: arithmetic and precedence", "[expr]") {
    REQUIRE(eval_ok("2 + 3 * 4") == 14.0);
    REQUIRE(eval_ok("(2 + 3) * 4") == 20.0);
    REQUIRE(eval_ok("2 ^ 3 ^ 2") == 512.0);   // right-associative
    REQUIRE(eval_ok("-2 ^ 2") == -4.0);        // unary minus looser than ^
    REQUIRE(eval_ok("2 ^ -2") == 0.25);
    REQUIRE(close_rel(eval_ok("3.75e7 / c_light^2"),
                      3.75e7 / (2.99792458e8 * 2.99792458e8)));
}

TEST_CASE("evaluate_pals_expression: functions", "[expr]") {
    REQUIRE(close(eval_ok("sqrt(2)"), std::sqrt(2.0)));
    REQUIRE(close(eval_ok("0.1*log(abs(-0.34))"), 0.1 * std::log(0.34)));
    REQUIRE(eval_ok("modulo(7, 3)") == 1.0);
    REQUIRE(eval_ok("floor(-1.5)") == -2.0);
    REQUIRE(eval_ok("ceiling(-1.5)") == -1.0);
    REQUIRE(eval_ok("int(-1.9)") == -1.0);     // toward zero
    REQUIRE(eval_ok("nint(2.5)") == 3.0);      // nearest
    REQUIRE(eval_ok("sign(-3)") == -1.0);
    REQUIRE(eval_ok("sinc(0)") == 1.0);
    REQUIRE(close(eval_ok("atan2(1, 1)"), std::atan(1.0)));
    REQUIRE(close(eval_ok("factorial(5)"), 120.0));
}

TEST_CASE("evaluate_pals_expression: built-in constants", "[expr]") {
    REQUIRE(close(eval_ok("pi"), 3.14159265358979323846));
    REQUIRE(eval_ok("c_light") == 2.99792458e8);
    // Value of apc::K_BOLTZMANN, in eV/K.
    REQUIRE(eval_ok("k_boltzmann") == 8.617333262e-5);

    // classical_radius_factor is derived rather than taken from APC, because
    // apc::CLASSICAL_RADIUS_FACTOR is 1e6 larger (its mass is in MeV while
    // apc::M_ELECTRON is in eV). Pin the exponent explicitly: the relation
    // below holds under either convention, so on its own it would not notice a
    // switch to the APC constant silently rescaling every lattice using it.
    REQUIRE(close_rel(eval_ok("classical_radius_factor"),
                      eval_ok("r_electron") *
                          eval_ok("mass_of(\"electron\")")));
    REQUIRE(close_rel(eval_ok("classical_radius_factor"),
                      1.4399645468825422e-9));

    // epsilon_0 and mu_0 are in the PALS standard's eV units — 1/(eV*m) and
    // eV*sec^2/m respectively, not the SI F/m and N/A^2. In these units the
    // identity eps_0 * mu_0 * c^2 == 1 holds, which pins both values at once.
    REQUIRE(close(eval_ok("epsilon_0"), 5.5263493618e7));
    REQUIRE(close_rel(eval_ok("mu_0"), 2.013354537e-25));
    REQUIRE(close(eval_ok("epsilon_0 * mu_0 * c_light^2"), 1.0));
}

TEST_CASE("evaluate_pals_expression: particle-data functions from libapc",
          "[expr]") {
    // Species names must always be quoted (single or double). Values mirror
    // AtomicAndPhysicalConstantsCLib (CODATA 2022).
    REQUIRE(close(eval_ok("mass_of(\"proton\")"), 938272089.43000007));
    REQUIRE(eval_ok("charge_of('electron')") == -1.0);
    REQUIRE(eval_ok("charge_of(\"anti-proton\")") == -1.0);
    REQUIRE(close(eval_ok("2 * mass_of(\"electron\")"), 2 * 510998.95069000003));
    // A mass number must carry a leading '#' (e.g. "#3He"). The bare atom is
    // neutral; the ionised form carries the charge.
    REQUIRE(eval_ok("charge_of(\"#3He\")") == 0.0);
    REQUIRE(eval_ok("charge_of(\"helion\")") == 2.0);
    REQUIRE(close(eval_ok("mass_of(\"#3He\")"), 2809413528.3197904));

    // An unquoted species name is an error.
    bool ok = true;
    evaluate_pals_expression("mass_of(electron)", &ok);
    REQUIRE_FALSE(ok);
    ok = true;
    evaluate_pals_expression("charge_of(#3He)", &ok);
    REQUIRE_FALSE(ok);
    // A mass number without the '#' prefix is an error, even when quoted.
    ok = true;
    evaluate_pals_expression("mass_of(\"3He\")", &ok);
    REQUIRE_FALSE(ok);
}

TEST_CASE("evaluate_pals_expression: expr() wrapper is accepted", "[expr]") {
    REQUIRE(eval_ok("expr(3.74 * 2)") == 7.48);
    REQUIRE(eval_ok("expr( (1 + 2) * 3 )") == 9.0);
}

TEST_CASE("evaluate_pals_expression: non-evaluable inputs report failure",
          "[expr]") {
    bool ok = true;
    // random()/random_gauss() are deferred, so not evaluable here.
    evaluate_pals_expression("0.01 + 0.003 * random_gauss()", &ok);
    REQUIRE_FALSE(ok);
    ok = true;
    evaluate_pals_expression("thingB", &ok);          // unknown identifier
    REQUIRE_FALSE(ok);
    ok = true;
    evaluate_pals_expression("mass_of(\"nonsense\")", &ok);  // unknown species
    REQUIRE_FALSE(ok);
    ok = true;
    evaluate_pals_expression("1 + ", &ok);            // parse error
    REQUIRE_FALSE(ok);
    ok = true;
    evaluate_pals_expression(nullptr, &ok);           // null input
    REQUIRE_FALSE(ok);
}

TEST_CASE("parse_and_expand_PALS evaluates expressions in the expanded tree",
          "[expr][lattices]") {
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - variables:\n"
        "        - a_var: 3.75e7 / c_light^2\n"
        "        - b_var: -0.34\n"
        "    - m_e:\n"
        "        kind: constant\n"
        "        value: mass_of(\"electron\")\n"
        "    - cleo:\n"
        "        kind: Solenoid\n"
        "        length: 0.1*log(abs(b_var))\n"
        "        MagneticMultipoleP:\n"
        "          Kn1: expr(3.74 * a_var)\n"
        "          Kn2: 0.01 + 0.003*random_gauss()\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - cleo\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    const double a_var = 3.75e7 / (2.99792458e8 * 2.99792458e8);

    // `cleo` is referenced by main_line, so expansion inlines its definition
    // into the lattice; this is the copy inside the expanded tree.
    YAMLNodeId cleo = find_by_key(lat.full_expanded, "cleo");
    REQUIRE(cleo != YAML_NULL_ID);

    // Immediate expression using a user variable.
    YAMLNodeId len = get_child_by_key(lat.full_expanded, cleo, "length");
    REQUIRE(close(num_val(lat.full_expanded, len), 0.1 * std::log(0.34)));

    YAMLNodeId mmp = get_child_by_key(lat.full_expanded, cleo, "MagneticMultipoleP");
    // expr()-delayed expression is evaluated to a number in the expanded tree.
    YAMLNodeId kn1 = get_child_by_key(lat.full_expanded, mmp, "Kn1");
    REQUIRE(close(num_val(lat.full_expanded, kn1), 3.74 * a_var));

    // random_gauss() is deferred: the text is left untouched.
    YAMLNodeId kn2 = get_child_by_key(lat.full_expanded, mmp, "Kn2");
    REQUIRE(val_eq(lat.full_expanded, kn2, "0.01 + 0.003*random_gauss()"));

    // Expressions are evaluated before the document is split, so a definition
    // that stayed behind is evaluated in adjunct just the same. `m_e` is not
    // referenced by the lattice, so adjunct is the only place it exists.
    YAMLNodeId m_e = facility_param(lat.adjunct, "m_e");
    REQUIRE(m_e != YAML_NULL_ID);
    YAMLNodeId m_e_val = get_child_by_key(lat.adjunct, m_e, "value");
    REQUIRE(close(num_val(lat.adjunct, m_e_val), 510998.95069000003));
    REQUIRE(find_by_key(lat.full_expanded, "m_e") == YAML_NULL_ID);

    // The combined tree keeps the original expression text (evaluation happens
    // downstream of it).
    YAMLNodeId c_cleo = facility_param(lat.combined, "cleo");
    YAMLNodeId c_len = get_child_by_key(lat.combined, c_cleo, "length");
    REQUIRE(val_eq(lat.combined, c_len, "0.1*log(abs(b_var))"));

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("parse_and_expand_PALS resolves compact constants/variables",
          "[expr][lattices]") {
    // The compact `constants:`/`variables:` lists (s:constants); a later
    // definition must be able to reference an earlier one by name.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - constants:\n"
        "        - a_const: 0.3 * r_electron\n"
        "        - b_const: 0.45\n"
        "    - variables:\n"
        "        - a_var: a_const^2\n"
        "        - b_var: 0.37 * atan2(0.1, 0.2)\n"
        "    - d1:\n"
        "        kind: Drift\n"
        "        length: a_const + b_const\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - d1\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    const double a_const = 0.3 * evaluate_pals_expression("r_electron", nullptr);

    // constants/variables blocks are not part of the lattice, so they are
    // adjunct — evaluated all the same.
    YAMLNodeId consts = facility_param(lat.adjunct, "constants");
    REQUIRE(close(num_val(lat.adjunct,
                          compact_def(lat.adjunct, consts, "a_const")),
                  a_const));

    // a_var references the constant a_const defined above it.
    YAMLNodeId vars = facility_param(lat.adjunct, "variables");
    REQUIRE(close(num_val(lat.adjunct,
                          compact_def(lat.adjunct, vars, "a_var")),
                  a_const * a_const));

    // An element parameter may reference the definitions too; this is
    // d1 as inlined into the expanded lattice.
    YAMLNodeId d1 = find_by_key(lat.full_expanded, "d1");
    REQUIRE(d1 != YAML_NULL_ID);
    REQUIRE(close(num_val(lat.full_expanded, get_child_by_key(lat.full_expanded, d1,
                                                         "length")),
                  a_const + 0.45));

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("parse_and_expand_PALS resolves element-parameter references",
          "[expr][lattices]") {
    // An expression may reference another element's parameter with the
    // `element>group.sub. ... .param` syntax; it resolves to that parameter's
    // value (evaluated as an expression in turn).
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - thingB:\n"
        "        kind: Sextupole\n"
        "        length: 0.3\n"
        "        MagneticMultipoleP:\n"
        "          Kn2L: 0.1\n"
        "    - DH1A:\n"
        "        kind: Bend\n"
        "        length: 0.2\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "        BendP:\n"
        "          edge2_int: 0.02 * thingB>MagneticMultipoleP.Kn2L\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - DH1A\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    YAMLNodeId dh1a = find_by_key(lat.full_expanded, "DH1A");
    REQUIRE(dh1a != YAML_NULL_ID);
    YAMLNodeId bendp = get_child_by_key(lat.full_expanded, dh1a, "BendP");
    REQUIRE(close(
        num_val(lat.full_expanded, get_child_by_key(lat.full_expanded, bendp, "edge2_int")),
        0.02 * 0.1));

    // A clean lattice reports no problems.
    REQUIRE(lat.problems.count == 0);
    free_lattice_problems(lat.problems);  // safe on an empty list

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("parse_and_expand_PALS resolves a species-name constant",
          "[expr][lattices]") {
    // A particle-data function may take a symbol whose value is a species name
    // (`mass_of(species)` where `species: "#3He"`), not only a quoted literal.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - constants:\n"
        "        - species: \"#3He\"\n"
        "        - b_const: 0.45 * mass_of(species)\n"
        "    - DH1A:\n"
        "        kind: Bend\n"
        "        ReferenceP:\n"
        "          species_ref: species\n"
        "          E_tot_ref: 1.0e10\n"
        "        BendP:\n"
        "          h1: 1.1 * mass_of(species)\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - DH1A\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    const double m_3he = 2809413528.3197904;  // mass_of("#3He"), CODATA 2022

    YAMLNodeId consts = facility_param(lat.adjunct, "constants");
    REQUIRE(close(num_val(lat.adjunct,
                          compact_def(lat.adjunct, consts, "b_const")),
                  0.45 * m_3he));

    YAMLNodeId dh1a = find_by_key(lat.full_expanded, "DH1A");
    REQUIRE(dh1a != YAML_NULL_ID);
    YAMLNodeId bendp = get_child_by_key(lat.full_expanded, dh1a, "BendP");
    REQUIRE(close(num_val(lat.full_expanded,
                          get_child_by_key(lat.full_expanded, bendp, "h1")),
                  1.1 * m_3he));

    // A bare identifier naming the species constant (`species_ref: species`) is
    // replaced by its species-name string in the expanded tree.
    YAMLNodeId refp = get_child_by_key(lat.full_expanded, dh1a, "ReferenceP");
    REQUIRE(val_eq(lat.full_expanded,
                   get_child_by_key(lat.full_expanded, refp, "species_ref"),
                   "#3He"));

    // The species constant itself stays as its (string) species name.
    REQUIRE(val_eq(lat.adjunct,
                   compact_def(lat.adjunct, consts, "species"), "#3He"));

    // No spurious problems.
    REQUIRE(lat.problems.count == 0);
    free_lattice_problems(lat.problems);

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

TEST_CASE("parse_and_expand_PALS leaves root prose alone", "[expr][lattices]") {
    // `authors`, `notes` and `reminders` are free-form prose (fundamentals.md,
    // s:palsroot). Prose carrying a `/` or parentheses reads as arithmetic to
    // looks_like_expression, so evaluating it at all would report a translated
    // file's provenance note as a broken expression.
    const char* doc =
        "PALS:\n"
        "  authors:\n"
        "    - \"D. Sagan (Cornell)\"\n"
        "  notes:\n"
        "    - \"Translated from /nfs/acc/user/lat.bmad\"\n"
        "  reminders:\n"
        "    - \"Phase the RF (west) before tracking\"\n"
        "  facility:\n"
        "    - DH1A:\n"
        "        kind: Bend\n"
        "        length: 0.2\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - DH1A\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    REQUIRE(lat.full_expanded != nullptr);

    REQUIRE(lat.problems.count == 0);
    free_lattice_problems(lat.problems);

    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}

namespace {

// Every problem reported for a one-element lattice whose `h1` is `value`.
std::vector<std::string> problems_for_value(const std::string& value) {
    const std::string doc =
        "PALS:\n"
        "  facility:\n"
        "    - DH1A:\n"
        "        kind: Bend\n"
        "        length: 0.2\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "        BendP:\n"
        "          h1: " + value + "\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - DH1A\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc.c_str(), nullptr);
    std::vector<std::string> out;
    for (size_t i = 0; i < lat.problems.count; ++i)
        out.push_back(lat.problems.items[i].message);
    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
    return out;
}

bool any_has(const std::vector<std::string>& ps, const char* needle) {
    for (const std::string& p : ps)
        if (p.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("an expression that fails to evaluate says why", "[expr][lattices]") {

    // The offending symbol is named. Without it the reader is left to find it
    // in the expression themselves, which is the whole difficulty when the
    // expression came out of a translator.
    std::vector<std::string> ps = problems_for_value("1.0 / C_LIGHT");
    REQUIRE(any_has(ps, "unknown constant or variable 'C_LIGHT'"));
    // Every built-in constant is lower case, so a miscased one is a spelling
    // the reader can act on.
    REQUIRE(any_has(ps, "did you mean 'c_light'?"));

    // A name that is nothing like a constant gets no invented suggestion.
    ps = problems_for_value("1.0 / bogus_thing");
    REQUIRE(any_has(ps, "unknown constant or variable 'bogus_thing'"));
    REQUIRE(!any_has(ps, "did you mean"));

    ps = problems_for_value("SQRT(2.0)");
    REQUIRE(any_has(ps, "unknown function 'SQRT'; did you mean 'sqrt'?"));

    // Arity is reported against the function, not as an unknown name.
    ps = problems_for_value("atan2(1.0)");
    REQUIRE(any_has(ps, "'atan2' takes two arguments, got 1"));

    ps = problems_for_value("mass_of(\"bogusium\")");
    REQUIRE(any_has(ps, "unknown species 'bogusium'"));

    ps = problems_for_value("mass_of(\"3He\")");
    REQUIRE(any_has(ps, "needs a leading '#' on its mass number"));

    ps = problems_for_value("2.0 * (3.0 + 4.0");
    REQUIRE(any_has(ps, "missing ')'"));
}

TEST_CASE("a controller says why an expression failed", "[expr][controllers]") {
    // The reason reaches the controller messages too, which is where a
    // translated lattice tends to put its expressions.
    const char* doc =
        "PALS:\n"
        "  facility:\n"
        "    - oo:\n"
        "        kind: Controller\n"
        "        control_type: ABSOLUTE\n"
        "        variables:\n"
        "          vv: -1.0 / C_LIGHT\n"
        "        controls:\n"
        "          - parameter: q1>MagneticMultipoleP.Kn1\n"
        "            expression: 4 ^ PI\n"
        "    - q1:\n"
        "        kind: Quadrupole\n"
        "        length: 0.2\n"
        "        ReferenceP:\n"
        "          species_ref: proton\n"
        "          E_tot_ref: 1.0e9\n"
        "    - main_line:\n"
        "        kind: BeamLine\n"
        "        line:\n"
        "          - q1\n"
        "    - lat1:\n"
        "        kind: Lattice\n"
        "        branches:\n"
        "          - main_line\n"
        "    - use: \"lat1\"\n";

    struct lattices lat = expand_PALS_string(doc, nullptr);
    std::vector<std::string> ps;
    for (size_t i = 0; i < lat.problems.count; ++i)
        ps.push_back(lat.problems.items[i].message);

    REQUIRE(any_has(ps, "variable 'vv': could not evaluate"));
    REQUIRE(any_has(ps, "unknown constant or variable 'C_LIGHT'"));
    REQUIRE(any_has(ps, "control expression could not be evaluated"));
    REQUIRE(any_has(ps, "unknown constant or variable 'PI'; did you mean 'pi'?"));

    free_lattice_problems(lat.problems);
    delete_tree(lat.original);
    delete_tree(lat.combined);
    delete_tree(lat.expanded);
    delete_tree(lat.full_expanded);
    delete_tree(lat.adjunct);
}
