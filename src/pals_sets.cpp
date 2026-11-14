// The `facility` and `post_expansion` lists: `set` commands, and the in-order
// walk of `facility` that resolves each definition's inherits and immediate
// expressions where it stands. See pals_pipeline.h.

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "pals_pipeline.h"
#include "pals_util.h"
#include "yaml_tree.h"

namespace pals {

// ============================================================
// SET COMMANDS, facility AND post_expansion
// ============================================================
//
// A `set` (parameter-values-and-controllers.md, s:set) writes a value into
// every parameter its `parameter` name-matching string selects:
//
//   - set:
//       parameter: B1.*>BendP.e1
//       value: 2*PARAMETER + atan(SELF>BendP.g_ref)
//
// In the `value` expression `PARAMETER` stands for the current value of the
// parameter being written and `SELF` for the element that owns it. The compact
// `sets:` form is a list of `target: value` pairs with no error terms.
//
// Where a set acts depends on which list holds it (top-level.md, s:facility). A
// set in `facility` acts on the element *definitions*, and only on those
// defined before it in the list, so one definition is written once and every
// expanded copy inherits the value. A set in `post_expansion` acts on the
// already-expanded lattice, so each copy of a repeated element is written
// separately.

static SetTarget make_target(size_t ele, const std::vector<std::string>& path) {
    SetTarget tg;
    tg.ele = ele;
    tg.path = path;
    tg.key = path.empty() ? "" : path.back();
    if (path.size() >= 2) tg.group = path[path.size() - 2];
    return tg;
}

// True when `path` names a parameter of `ele` that is not written but is still
// to be derived from a family member that is -- reading it is an error rather
// than reading zero. Only meaningful before the bookkeeper has run.
static bool awaits_derivation(const ryml::Tree& t, size_t ele,
                              const std::vector<std::string>& path) {
    if (path.empty()) return false;
    if (resolve_param_path(t, ele, path) != ryml::NONE) return false;
    SetTarget tg = make_target(ele, path);
    std::set<std::string> family = linked_family(tg.group, tg.key);
    if (family.empty()) return false;
    std::vector<std::string> sib = path;
    for (const std::string& member : family) {
        if (member == tg.key) continue;
        sib.back() = member;
        if (resolve_param_path(t, ele, sib) != ryml::NONE) return true;
    }
    return false;
}

// The parameters whose default value is `null` (one section each under
// source/parameters). `PARAMETER` (s:set) is the current value of the parameter
// being written; for an unwritten parameter that is its default, and a `null`
// default is no value at all, so reading it is an error. This matters most for
// a BeamLine line item that only uses an element: before branch expansion it
// holds just the values written on the item itself, not the definition's.
static bool has_null_default(const std::vector<std::string>& path) {
    static const std::map<std::string, std::set<std::string>> nulls = {
        {"ApertureP",
         {"x_min", "x_max", "x_center", "x_width", "y_min", "y_max", "y_center",
          "y_width", "vertices"}},
        {"BeamBeamP",
         {"sigma_x", "sigma_y", "sigma_z", "alpha_x", "beta_x", "alpha_y",
          "beta_y", "charge", "energy", "N_particle"}},
        {"BendP",
         {"Bn0_ref", "e1", "e2", "e1_rect", "e2_rect", "g_ref", "L_chord",
          "L_rectangle", "radius_ref"}},
        {"FloorP", {"x", "y", "z", "theta", "phi", "psi"}},
        {"ParticleP",
         {"sigma_xx", "sigma_xpx", "sigma_xy", "sigma_xpy", "sigma_xz",
          "sigma_xpz", "sigma_pxpx", "sigma_pxy", "sigma_pxpy", "sigma_pxz",
          "sigma_pxpz", "sigma_yy", "sigma_ypy", "sigma_yz", "sigma_ypz",
          "sigma_pypy", "sigma_pyz", "sigma_pypz", "sigma_zz", "sigma_zpz",
          "sigma_pzpz"}},
        {"ReferenceP", {"species_ref"}},
        {"ReferenceChangeP",
         {"dtime_ref", "dE_ref", "dpc_ref", "time_ref", "E_tot_ref", "pc_ref",
          "species_ref"}},
        {"RFP", {"frequency", "harmonic_number", "num_cells"}},
    };
    if (path.size() < 2) return false;
    auto it = nulls.find(path[path.size() - 2]);
    return it != nulls.end() && it->second.count(path.back()) != 0;
}

// Reads the value a `set` expression sees for a parameter: the written value if
// there is one, otherwise zero (an unwritten parameter of a known element is
// zero, per s:lattice.expand). `pending` reports the third case -- the value is
// still to be derived, so reading it is an error. When `want_default` is set
// (the read is for `PARAMETER`), `null_default` reports a fourth: the parameter
// is unwritten and its default is `null`, so there is no value to read.
static bool read_set_operand(const ryml::Tree& t, size_t ele,
                             const std::vector<std::string>& path,
                             const EvalContext& ctx, bool pre, double& out,
                             bool& pending, bool want_default,
                             bool& null_default) {
    pending = false;
    null_default = false;
    if (ele == ryml::NONE || path.empty()) return false;
    size_t node = resolve_param_path(t, ele, path);
    if (node != ryml::NONE && t.has_val(node)) {
        bool was_expr = false;
        std::string body = strip_expr_wrapper(val_str(t, node), was_expr);
        EvalOutcome r = eval_expression(body, ctx.resolve, ctx.species);
        if (!r.ok) return false;
        out = r.value;
        return true;
    }
    if (pre && awaits_derivation(t, ele, path)) {
        pending = true;
        return false;
    }
    if (want_default && has_null_default(path)) {
        null_default = true;
        return false;
    }
    out = 0.0;
    return true;
}

// One `set` command, read out of the tree.
struct SetCommand {
    std::string param;   // `parameter`: the name-matching target
    std::string value;   // `value`: the expression to write
    // The node holding `value`: what the written parameter is linked back to
    // (its provenance), so the correspondence map leads from the parameter to
    // the expression that set it rather than to the value it replaced.
    size_t value_node = ryml::NONE;
    double abs_error = 0.0;
    double rel_error = 0.0;
    bool has_error = false;
};

// Give a bare line reference somewhere to hold a parameter, returning the node
// the parameter hangs off. `- q1` and `- q1:` are the same entry -- the second
// just has room for values of its own -- so a `set` naming the first rewrites it
// into the second rather than refusing the write. The node comes back unchanged
// when it is already keyed, which every other kind of match is.
//
// The new nodes carry no provenance, like the ReferenceP/FloorP the bookkeeper
// adds: they answer to nothing in the source, since the source did not write
// them.
static size_t materialize_line_reference(ryml::Tree& t, size_t node) {
    if (node == ryml::NONE || t.has_key(node) || !t.has_val(node)) return node;
    std::string name = val_str(t, node);
    t.change_type(node, ryml::MAP);  // the entry becomes the anonymous wrapper map
    size_t def = t.append_child(node);
    t.to_map(def, t.to_arena(ryml::to_csubstr(name)));
    return def;
}

// Execute one `set` against the elements `matches` selected. `pre` distinguishes
// a pre-expansion set (acting on definitions, where a derived value is not yet
// available) from a post-expansion one (acting on the expanded lattice, where
// writing a parameter makes its family stale).
//
// `written`, when given, collects one target per parameter actually written --
// what the `expanded` tree needs in order to tell a post-expansion set's input
// apart from a value the bookkeeper derived from it (see AuthoredParams).
static void execute_set(ryml::Tree& t, const SetCommand& cmd,
                        const ElementMatches& matches, bool pre,
                        const EvalContext& ctx, Provenance& prov,
                        ProblemList& problems,
                        std::vector<SetTarget>* written = nullptr) {
    std::string what = "set '" + cmd.param + "'";

    if (!matches.valid) {
        add_problem(problems, what + ": malformed parameter target");
        return;
    }
    if (!matches.has_param || matches.path.empty() ||
        matches.path.back().empty()) {
        add_problem(problems, what + ": target names no element parameter");
        return;
    }
    if (matches.elements.empty()) {
        add_problem(problems, what + ": target matches nothing" +
                                  (pre ? " defined before it" : ""));
        return;
    }
    // "the true error is absolute_error + relative_error * |value|" -- but the
    // standard does not say how that error is distributed, and this library
    // never invents randomness (random()/random_gauss() are deferred for the
    // same reason). The deterministic value is written and the error is not.
    if (cmd.has_error)
        add_problem(problems,
                    what +
                        ": absolute_error/relative_error are not "
                        "applied -- the standard does not specify "
                        "the error distribution",
                    cmd.param, Severity::Warning, Origin::Unspecified);

    // A delayed value (s:set) is written as the expression itself, so it is
    // evaluated with the other delayed expressions. It may not use
    // `PARAMETER`: the parameter would then be defined by itself, an endless
    // evaluation loop. `SELF` names the element the set is acting on, which a
    // stored expression has no way to spell, so such a value is evaluated now.
    bool was_delayed = false;
    std::string delayed_body = strip_expr_wrapper(cmd.value, was_delayed);
    if (was_delayed) {
        bool uses_self = false;
        for (const std::string& id : expression_identifiers(delayed_body)) {
            if (id == "PARAMETER") {
                add_problem(problems,
                            what + ": a delayed value may not use PARAMETER "
                                   "(the parameter would be defined by itself)");
                return;
            }
            if (id == "SELF") uses_self = true;
        }
        if (uses_self) {
            add_problem(problems,
                        what + ": a delayed value using SELF is evaluated once, "
                               "as an immediate expression",
                        cmd.param, Severity::Warning, Origin::Unspecified);
            was_delayed = false;
        }
    }

    // Only a pre-expansion set needs the element map, to spot a reference to a
    // parameter whose value has still to be derived.
    std::map<std::string, size_t> emap;
    if (pre) make_ele_map(emap, t, t.root_id());

    for (size_t match : matches.elements) {
        size_t ele = materialize_line_reference(t, match);
        SetTarget tg = make_target(ele, matches.path);
        std::string ename = key_str(t, ele);
        std::string where = what + " on '" + ename + "'";

        // `PARAMETER` is the value being replaced; `SELF>path` reaches the
        // rest of the element. Both are scoped to this one element, so the
        // resolver is rebuilt for each. A value still to be derived is noted
        // under the name it was read by, `label`. `PARAMETER` alone also
        // reports an unwritten parameter whose default is `null`.
        bool pending = false;
        std::string pending_name;
        bool no_default = false;
        auto operand = [&](size_t of, const std::vector<std::string>& path,
                           const std::string& label, double& out,
                           bool want_default = false) {
            bool p = false;
            bool nd = false;
            bool ok = read_set_operand(t, of, path, ctx, pre, out, p,
                                       want_default, nd);
            if (p) {
                pending = true;
                pending_name = label;
            }
            if (nd) no_default = true;
            return ok;
        };
        SymbolLookup scoped = [&](const std::string& name,
                                  double& out) -> bool {
            if (name == "PARAMETER")
                return operand(ele, tg.path, matches.path.back(), out, true);
            if (name.compare(0, 5, "SELF>") == 0)
                return operand(ele, split_dots(name.substr(5)), name, out);
            // An `element>path` reference reads by the same rules: the written
            // value, else zero, else -- when a family member is written, so the
            // value is still to be derived -- an error.
            size_t gt = name.find('>');
            if (pre && gt != std::string::npos) {
                auto it = emap.find(name.substr(0, gt));
                if (it != emap.end())
                    return operand(it->second, split_dots(name.substr(gt + 1)),
                                   name, out);
            }
            return ctx.resolve ? ctx.resolve(name, out) : false;
        };

        bool was_expr = false;
        std::string body = strip_expr_wrapper(cmd.value, was_expr);
        EvalOutcome r = eval_expression(body, scoped, ctx.species);
        if (pending) {
            add_problem(problems, where + ": '" + pending_name +
                                      "' has no value yet -- it is derived "
                                      "during lattice expansion");
            continue;
        }
        if (no_default) {
            add_problem(problems, where + ": PARAMETER has no value -- '" +
                                      matches.path.back() +
                                      "' is not set and its default is null");
            continue;
        }
        if (r.deferred) continue;  // random(); leave the parameter alone
        if (!r.ok) {
            std::string msg = where + ": could not evaluate value: " + cmd.value;
            if (!r.error.empty()) msg += " -- " + r.error;
            add_problem(problems, msg);
            continue;
        }

        size_t parent = ele;
        for (size_t i = 0; i + 1 < tg.path.size(); ++i)
            parent = find_or_add_map_child(t, parent, tg.path[i].c_str());
        if (was_delayed)
            set_plain_child(t, parent, tg.key.c_str(),
                            "expr(" + delayed_body + ")");
        else
            set_num_child(t, parent, tg.key.c_str(), r.value);
        link_to_writer(t.find_child(parent, ryml::to_csubstr(tg.key)),
                       cmd.value_node, prov);
        if (written) written->push_back(tg);
        // This write nullifies the previous settings of the rest of the family:
        // whatever the definition stated before expansion, and -- after it --
        // whatever the first bookkeeper pass derived from that.
        nullify_family(t, ele, tg.path, prov);
    }
}

// Read a `set` node into a SetCommand. Returns false (with a problem recorded)
// when the required components are missing.
static bool read_set_command(const ryml::Tree& t, size_t node, SetCommand& cmd,
                             ProblemList& problems) {
    cmd.param = child_val_str(t, node, "parameter");
    cmd.value = child_val_str(t, node, "value");
    cmd.value_node = t.find_child(node, ryml::to_csubstr("value"));
    if (cmd.param.empty()) {
        add_problem(problems, "set: no `parameter` target");
        return false;
    }
    if (cmd.value.empty()) {
        add_problem(problems, "set '" + cmd.param + "': no `value`");
        return false;
    }
    double e = 0.0;
    if (get_num_child(t, node, "absolute_error", e) && e != 0.0) {
        cmd.abs_error = e;
        cmd.has_error = true;
    }
    if (get_num_child(t, node, "relative_error", e) && e != 0.0) {
        cmd.rel_error = e;
        cmd.has_error = true;
    }
    return true;
}

// The set commands a facility entry holds: one for a `set` node, one per pair
// for the compact `sets` list. Anything else yields none.
static std::vector<SetCommand> entry_set_commands(const ryml::Tree& t,
                                                  size_t entry,
                                                  ProblemList& problems) {
    std::vector<SetCommand> out;
    size_t c = entry_content(t, entry);
    if (c == ryml::NONE) return out;
    std::string key = key_str(t, c);

    if (key == "set") {
        SetCommand cmd;
        if (read_set_command(t, c, cmd, problems)) out.push_back(std::move(cmd));
        return out;
    }
    if (key != "sets") return out;

    // Compact form: a list of `target: value` pairs (written as a sequence of
    // single-key maps, or as a plain map).
    for_each_pair(t, c, [&](size_t kv) {
        if (!t.has_key(kv) || !t.has_val(kv)) return;
        SetCommand cmd;
        cmd.param = key_str(t, kv);
        cmd.value = val_str(t, kv);
        cmd.value_node = kv;
        out.push_back(std::move(cmd));
    });
    return out;
}

// Resolve the `inherit`s of the facility definition `def` -- the element's own
// and those of its parameter groups -- against the definitions made before it.
// Inheritance is immediate (lattice-elements.md): the values are copied into
// `def` now, so a `set` further down the list acting on the element inherited
// from does not reach `def`, and the `inherit` is spent -- removed, so expansion
// does not copy again. An inherit naming something defined only later in the
// list is an error; one naming nothing at all is left for expand to report.
static void resolve_definition_inherits(
    ryml::Tree& t, size_t def, const std::map<std::string, size_t>& before,
    const std::map<std::string, size_t>& everything,
    Provenance& prov, ProblemList& problems) {
    auto is_line = [&](size_t n) {
        std::string kind = child_val_str(t, n, "kind");
        return kind == "BeamLine" || kind == "Lattice";
    };
    auto resolve = [&](size_t node, const std::string& where) {
        size_t inh = t.find_child(node, ryml::to_csubstr("inherit"));
        if (inh == ryml::NONE || !t.has_val(inh)) return;
        std::string name = val_str(t, inh);
        size_t src = inherit_source(t, before, name);
        if (src != ryml::NONE && is_line(src)) return;  // a line: expansion's
        if (src == ryml::NONE) {
            if (inherit_source(t, everything, name) != ryml::NONE)
                add_problem(problems, where + ": inherit: '" + name +
                                          "' must be defined before it");
            return;
        }
        ensure_capacity(t, t.num_children(src) + 1);
        merge_children_no_rep_tracked(t, src, node, prov);
        remove_tracked(t, inh, prov);
    };
    // An element: its own inherit, then its groups'.
    std::function<void(size_t)> element = [&](size_t e) {
        std::string ename = key_str(t, e);
        resolve(e, "'" + ename + "'");
        for (size_t g = t.first_child(e); g != ryml::NONE; g = t.next_sibling(g))
            if (t.is_map(g) && t.has_key(g))
                resolve(g, "'" + ename + "' " + key_str(t, g));
    };
    // The items of a `line`, a `branches` list or a UnionEle's `elements`
    // (beamlines.md): an item that defines an element in place -- with a
    // `kind` or an `inherit` of its own -- inherits now, like any definition.
    // An item that only uses an element defined elsewhere gets that element's
    // parameters at branch expansion (names.md), so it is left alone here, as
    // are sub-lines.
    std::function<void(size_t)> items = [&](size_t n) {
        for (size_t c = t.first_child(n); c != ryml::NONE; c = t.next_sibling(c)) {
            if (!t.is_seq(c) || !t.has_key(c)) continue;
            std::string key = key_str(t, c);
            if (key != "line" && key != "branches" && key != "elements") continue;
            for (size_t e = t.first_child(c); e != ryml::NONE;
                 e = t.next_sibling(e)) {
                size_t item = entry_content(t, e);
                if (item == ryml::NONE || !t.is_map(item)) continue;
                std::string iname = key_str(t, item);
                auto b = before.find(iname);
                bool names_line = b != before.end() && is_line(b->second);
                // A branch's `inherit` names its root line, for expansion.
                if (key != "branches" && !is_line(item) && !names_line)
                    element(item);
                items(item);
            }
        }
    };
    // An inherit on a BeamLine or Lattice names a line, which expansion
    // resolves.
    if (!is_line(def)) element(def);
    items(def);
}

// Walk the `facility` list in order (s:lattice.expand): each definition has its
// inherits resolved and its immediate expressions evaluated as it is reached,
// and each `set` acts on the definitions before it. So an immediate expression
// keeps the value it had at its point in the list, while an `expr(...)` delayed
// one is left for the evaluation after expansion. What an immediate expression
// cannot evaluate yet -- it may need a parameter expansion computes -- is left
// too, and evaluated (or reported) after expansion.
void run_facility_list(ryml::Tree& t, const std::vector<size_t>& entries,
                       Provenance& prov, ProblemList& problems) {
    std::map<std::string, size_t> everything;
    make_ele_map(everything, t, t.root_id());

    EvalContext ctx = make_expression_context(t);
    SubstituteMode immediate;
    immediate.immediate_only = true;

    std::map<std::string, size_t> before;  // facility definitions so far
    std::vector<size_t> defined;
    for (size_t entry : entries) {
        std::vector<SetCommand> cmds = entry_set_commands(t, entry, problems);
        if (cmds.empty()) {
            size_t def = entry_content(t, entry);
            if (def != ryml::NONE && t.is_map(def)) {
                resolve_definition_inherits(t, def, before, everything, prov,
                                            problems);
                std::string name = key_str(t, def);
                if (t.find_child(def, ryml::to_csubstr("kind")) != ryml::NONE)
                    before.emplace(name, def);
            }
            substitute_values(t, entry, ctx, problems, immediate);
            defined.push_back(entry);
            continue;
        }
        for (const SetCommand& cmd : cmds) {
            execute_set(t, cmd,
                        match_definition_parameters(t, defined, cmd.param), true,
                        ctx, prov, problems);
            // A fresh context after each set: it wrote to the tree the next
            // expression reads, so a memoized value from before it is stale.
            ctx = make_expression_context(t);
        }
    }
}

// Run the `set` commands of the `post_expansion` list against the expanded
// lattice. Order still matters -- a later set sees what an earlier one wrote --
// but every set sees the whole lattice, since it has already been built.
void run_post_expansion_sets(ryml::Tree& t, size_t lat_node,
                             const std::vector<size_t>& entries,
                             Provenance& prov, ProblemList& problems,
                             std::vector<SetTarget>* written) {
    if (lat_node == ryml::NONE) return;
    for (size_t e : entries) {
        for (const SetCommand& cmd : entry_set_commands(t, e, problems)) {
            execute_set(t, cmd,
                        match_element_parameters(t, lat_node, cmd.param), false,
                        make_expression_context(t, lat_node), prov, problems,
                        written);
        }
    }
}

}  // namespace pals
