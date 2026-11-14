// Controllers (parameter-values-and-controllers.md, s:controller): reading the
// `kind: Controller` elements, evaluating them from the top of their hierarchy
// down, and applying the ABSOLUTE ones to the lattice parameters they drive.
// Run as part of evaluate_expressions; see pals_pipeline.h.

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "pals_pipeline.h"
#include "pals_util.h"
#include "yaml_tree.h"

namespace pals {

namespace {

// A controller `variables` entry awaiting evaluation.
struct CtrlVar {
    size_t node = ryml::NONE;  // scalar value node to overwrite in place
    size_t ctrl = 0;           // index into the controllers vector
    std::string name;          // unqualified name, as used in the controller
    std::string text;          // initial-value expression ("" = the default, 0)
    std::string error;         // why `text` did not evaluate, for the report
    bool done = false;
    // Set when an ABSOLUTE controller above this one in the hierarchy drives
    // this variable: the driving value replaces the initial value.
    bool driven = false;
    double driven_value = 0.0;
};

// A controller `controls` entry: a `parameter` target and the `expression`
// giving the value it is driven to.
struct CtrlControl {
    size_t expr_node = ryml::NONE;  // `expression` scalar, rewritten in place
    std::string param;              // `parameter` target spec
    std::string text;               // expression source
    bool ok = false;                // evaluated to a number
    double value = 0.0;
    // Set when `param` names another controller's variable rather than a
    // lattice parameter; that is what makes controllers a hierarchy.
    size_t target_ctrl = SIZE_MAX;
    std::string target_var;
};

// One `kind: Controller` element.
struct Ctrl {
    size_t node = ryml::NONE;
    std::string name;
    bool absolute = true;      // control_type: ABSOLUTE, the default
    std::vector<size_t> vars;  // indices into the flat CtrlVar list
    std::vector<CtrlControl> controls;
};

// Every controller of the document, with the lookups the passes below share.
struct Controllers {
    std::vector<Ctrl> ctrls;
    std::vector<CtrlVar> vars;
    std::map<std::string, size_t> by_name;  // controller name -> index
    // (controller, variable) -> index into `vars`.
    std::map<std::pair<size_t, std::string>, size_t> var_of;

    // The variable `name` as seen from controller `ci`: one of its own
    // (`cur1`), or another controller's (`ps27>cur1`). SIZE_MAX if neither.
    size_t var_named(size_t ci, const std::string& name) const {
        size_t gt = name.find('>');
        if (gt == std::string::npos) {
            auto it = var_of.find({ci, name});
            return it == var_of.end() ? SIZE_MAX : it->second;
        }
        auto ct = by_name.find(name.substr(0, gt));
        if (ct == by_name.end()) return SIZE_MAX;
        auto it = var_of.find({ct->second, name.substr(gt + 1)});
        return it == var_of.end() ? SIZE_MAX : it->second;
    }
};

// What the ABSOLUTE controllers add up to for one lattice parameter, keyed on
// (element definition, dotted path) so a parameter an element does not carry
// yet still has an identity.
struct CtrlTarget {
    double sum = 0.0;
    bool has_absolute = false;
    bool has_relative = false;
    bool deferred = false;  // some driving expression stayed unevaluated
    // Position of the last `controls` entry naming this parameter, in
    // evaluation order. Two controllers driving two members of one
    // interdependent family are ordered by it: the later write is the one that
    // states the quantity, and it nullifies the earlier.
    size_t seq = 0;
    // The `expression` nodes of every control reaching this parameter,
    // ABSOLUTE and RELATIVE, in evaluation order.
    std::vector<size_t> expr_nodes;
};

using TargetMap = std::map<std::pair<size_t, std::string>, CtrlTarget>;

}  // namespace

// Collects every `kind: Controller` node in the subtree.
static void collect_controllers(const ryml::Tree& t, size_t node,
                                std::vector<size_t>& out) {
    if (node == ryml::NONE) return;
    if (is_controller(t, node)) {
        out.push_back(node);
        return;  // controllers do not nest
    }
    for (size_t c = t.first_child(node); c != ryml::NONE;
         c = t.next_sibling(c))
        collect_controllers(t, c, out);
}

// Trimmed scalar value of a node; "" when the node holds nothing. A YAML null
// (`~`, `null`) reads as "no value given" the same as an empty scalar does.
static std::string scalar_text(const ryml::Tree& t, size_t node) {
    std::string v = trim(val_str(t, node));
    return (v == "~" || v == "null") ? "" : v;
}

// Reads one `kind: Controller` into `cs`, materializing the `control_type`
// default and rejecting the expressions the standard does not allow.
static void read_controller(ryml::Tree& t, size_t node, Controllers& cs,
                            ProblemList& problems) {
    Ctrl c;
    c.node = node;
    c.name = key_str(t, node);

    std::string ct =
        scalar_text(t, t.find_child(node, ryml::to_csubstr("control_type")));
    if (ct.empty()) {
        set_plain_child(t, node, "control_type", "ABSOLUTE");
    } else if (ct == "RELATIVE") {
        c.absolute = false;
    } else if (ct != "ABSOLUTE") {
        add_problem(problems, "controller '" + c.name +
                                  "': control_type must be ABSOLUTE or "
                                  "RELATIVE, not " + ct);
    }

    // The `variables`, in both the documented map form (`cur1: 0.023`) and the
    // compact seq-of-single-key-maps form. A variable written with no value at
    // all (`cur1:`) counts too: its value is the default, zero.
    size_t vnode = t.find_child(node, ryml::to_csubstr("variables"));
    for_each_pair(t, vnode, [&](size_t vn) {
        if (!t.has_key(vn) || t.is_map(vn) || t.is_seq(vn)) return;
        CtrlVar v;
        v.node = vn;
        v.ctrl = cs.ctrls.size();
        v.name = key_str(t, vn);
        v.text = scalar_text(t, vn);
        // An initial value is an immediate expression (s:controller), so an
        // `expr(...)` wrapper on it is an error.
        bool was_expr = false;
        strip_expr_wrapper(v.text, was_expr);
        if (was_expr) {
            add_problem(problems, "controller '" + c.name + "' variable '" +
                                      v.name +
                                      "': an initial value may not be wrapped "
                                      "in expr(...)");
            v.text.clear();
        }
        c.vars.push_back(cs.vars.size());
        cs.vars.push_back(std::move(v));
    });

    size_t controls = t.find_child(node, ryml::to_csubstr("controls"));
    if (controls != ryml::NONE) {
        for (size_t entry = t.first_child(controls); entry != ryml::NONE;
             entry = t.next_sibling(entry)) {
            if (!t.is_map(entry)) continue;
            CtrlControl cc;
            cc.param = scalar_text(
                t, t.find_child(entry, ryml::to_csubstr("parameter")));
            cc.expr_node = t.find_child(entry, ryml::to_csubstr("expression"));
            cc.text = scalar_text(t, cc.expr_node);
            if (cc.param.empty()) {
                add_problem(problems, "controller '" + c.name +
                                          "': a controls entry has no "
                                          "`parameter` target");
                continue;
            }
            if (cc.expr_node == ryml::NONE) {
                add_problem(problems, "controller '" + c.name +
                                          "': controls entry '" + cc.param +
                                          "' has no `expression`");
                continue;
            }
            c.controls.push_back(std::move(cc));
        }
    }
    cs.ctrls.push_back(std::move(c));
}

// Reads every controller in the tree, then splits each `parameter` target into
// the two things it can name. A target whose part before the `>` is a
// controller name is a controller variable (`ps27>cur1`); anything else is a
// lattice parameter.
static Controllers read_controllers(ryml::Tree& t, ProblemList& problems) {
    Controllers cs;
    std::vector<size_t> nodes;
    collect_controllers(t, t.root_id(), nodes);
    for (size_t node : nodes) read_controller(t, node, cs, problems);

    for (size_t ci = 0; ci < cs.ctrls.size(); ++ci)
        cs.by_name[cs.ctrls[ci].name] = ci;
    for (size_t vi = 0; vi < cs.vars.size(); ++vi)
        cs.var_of[{cs.vars[vi].ctrl, cs.vars[vi].name}] = vi;

    for (Ctrl& c : cs.ctrls) {
        for (CtrlControl& cc : c.controls) {
            size_t gt = cc.param.find('>');
            if (gt == std::string::npos) continue;
            auto it = cs.by_name.find(cc.param.substr(0, gt));
            if (it == cs.by_name.end()) continue;
            std::string var = cc.param.substr(gt + 1);
            if (!cs.var_of.count({it->second, var})) {
                add_problem(problems, "controller '" + c.name + "' controls '" +
                                          cc.param + "': controller '" +
                                          it->first + "' has no variable '" +
                                          var + "'");
                continue;
            }
            cc.target_ctrl = it->second;
            cc.target_var = var;
        }
    }
    return cs;
}

// True if expression `text` reads a variable of the controller `ctrl`, which it
// spells `{ctrl}>{variable}` (s:controller).
static bool reads_controller(const std::string& text, const std::string& ctrl) {
    if (ctrl.empty()) return false;
    const std::string pat = ctrl + ">";
    for (size_t at = text.find(pat); at != std::string::npos;
         at = text.find(pat, at + 1)) {
        char before = at == 0 ? ' ' : text[at - 1];
        if (std::isalnum(static_cast<unsigned char>(before)) || before == '_' ||
            before == '>')
            continue;
        return true;
    }
    return false;
}

// Orders the controllers so that every controller comes before the ones whose
// variables it drives, which is the "start at the top of the hierarchy and work
// downwards" evaluation the standard calls for. Controllers not related by
// `controls` keep their file order, so the result does not depend on how the
// file is written. A cycle is a file error: it is reported, and the controllers
// caught in it are appended in file order so evaluation still produces
// something for everything outside the cycle.
static std::vector<size_t> controller_order(const std::vector<Ctrl>& ctrls,
                                            ProblemList& problems) {
    std::vector<std::vector<size_t>> below(ctrls.size());
    std::vector<size_t> indeg(ctrls.size(), 0);
    auto edge = [&](size_t from, size_t to) {
        below[from].push_back(to);
        ++indeg[to];
    };
    for (size_t ci = 0; ci < ctrls.size(); ++ci)
        for (const CtrlControl& c : ctrls[ci].controls) {
            if (c.target_ctrl != SIZE_MAX) edge(ci, c.target_ctrl);
            // A control expression reading another controller's variable needs
            // that controller -- and whatever drives it -- evaluated first.
            for (size_t cj = 0; cj < ctrls.size(); ++cj)
                if (cj != ci && reads_controller(c.text, ctrls[cj].name))
                    edge(cj, ci);
        }

    std::vector<size_t> order;
    std::vector<bool> queued(ctrls.size(), false);
    for (size_t pass = 0; pass < ctrls.size(); ++pass) {
        bool progress = false;
        for (size_t ci = 0; ci < ctrls.size(); ++ci) {
            if (queued[ci] || indeg[ci] != 0) continue;
            queued[ci] = true;
            order.push_back(ci);
            for (size_t cj : below[ci]) --indeg[cj];
            progress = true;
        }
        if (!progress) break;
    }

    if (order.size() != ctrls.size()) {
        std::string names;
        for (size_t ci = 0; ci < ctrls.size(); ++ci)
            if (!queued[ci]) {
                if (!names.empty()) names += ", ";
                names += "'" + ctrls[ci].name + "'";
                order.push_back(ci);
            }
        add_problem(problems,
                    "controllers form a circular control hierarchy: " + names);
    }
    return order;
}

// Evaluates every controller, in `order`, writing each variable's value and
// each control expression's value back into the tree.
//
// Each controller's `variables` are a symbol table scoped to that controller.
// An initial value is an immediate expression and may use any variable,
// constant or lattice parameter; a variable written with no value is zero.
// Controllers are walked from the top of the hierarchy downwards, so a variable
// driven from above already holds its driving value by the time the controller
// owning it is evaluated.
static void evaluate_in_order(ryml::Tree& t, Controllers& cs,
                              const std::vector<size_t>& order,
                              const EvalContext& global,
                              ProblemList& problems,
                              std::vector<DelayedValue>* delayed) {
    std::vector<Ctrl>& ctrls = cs.ctrls;
    std::vector<CtrlVar>& vars = cs.vars;

    // Initial values are immediate expressions (s:controller): each is
    // evaluated once, and may use constants, variables -- this controller's or
    // another's, by their initial values -- and lattice parameters. Evaluated
    // on demand, so the order the variables are written in does not matter;
    // a circular set of initial values does not evaluate.
    std::map<size_t, EvalOutcome> init_memo;
    std::set<size_t> init_active;
    std::function<EvalOutcome(size_t)> init_value =
        [&](size_t vi) -> EvalOutcome {
        auto m = init_memo.find(vi);
        if (m != init_memo.end()) return m->second;
        EvalOutcome r;
        if (vars[vi].text.empty()) {  // no value given: the default is zero
            r.ok = true;
            r.value = 0.0;
            return init_memo[vi] = r;
        }
        if (!init_active.insert(vi).second) {
            r.error = "circular initial values";
            return r;
        }
        size_t ci = vars[vi].ctrl;
        // Whether a variable this one reads is caught in a cycle, so the
        // failure is reported as the cycle rather than as an unknown name.
        bool circular = false;
        SymbolLookup lookup = [&, ci](const std::string& name,
                                      double& out) -> bool {
            size_t vj = cs.var_named(ci, name);
            if (vj != SIZE_MAX) {
                EvalOutcome o = init_value(vj);
                if (!o.ok) {
                    if (o.error == "circular initial values") circular = true;
                    return false;
                }
                out = o.value;
                return true;
            }
            return global.resolve ? global.resolve(name, out) : false;
        };
        bool was_expr = false;
        std::string body = strip_expr_wrapper(vars[vi].text, was_expr);
        r = eval_expression(body, lookup, global.species);
        if (!r.ok && circular) r.error = "circular initial values";
        init_active.erase(vi);
        return init_memo[vi] = r;
    };

    // Symbol tables, filled in as each controller is reached. A control
    // expression sees its own controller's variables, the variables of the
    // controllers evaluated before it (`ps27>cur1`), and everything global.
    std::vector<std::map<std::string, double>> locals(ctrls.size());
    auto resolver_for = [&](size_t ci) {
        return SymbolLookup([ci, &locals, &global, &cs](const std::string& name,
                                                        double& out) -> bool {
            auto it = locals[ci].find(name);
            if (it != locals[ci].end()) {
                out = it->second;
                return true;
            }
            size_t gt = name.find('>');
            if (gt != std::string::npos) {
                auto ct = cs.by_name.find(name.substr(0, gt));
                if (ct != cs.by_name.end()) {
                    auto v = locals[ct->second].find(name.substr(gt + 1));
                    if (v == locals[ct->second].end()) return false;
                    out = v->second;
                    return true;
                }
            }
            return global.resolve ? global.resolve(name, out) : false;
        });
    };

    for (size_t ci : order) {
        Ctrl& c = ctrls[ci];
        SymbolLookup res = resolver_for(ci);

        // Build this controller's symbol table. A driven variable takes the
        // value the controllers above set -- that is what it replaced its own
        // initial value with. Every other one takes its initial value.
        for (size_t vi : c.vars) {
            CtrlVar& v = vars[vi];
            if (v.driven) {
                locals[ci][v.name] = v.driven_value;
                set_scalar_num(t, v.node, v.driven_value);
                v.done = true;
                continue;
            }
            EvalOutcome r = init_value(vi);
            if (r.ok) {
                locals[ci][v.name] = r.value;
                set_scalar_num(t, v.node, r.value);
                v.done = true;
            } else if (r.deferred) {
                v.done = true;  // random(); leave the text untouched
            } else {
                v.error = r.error;
            }
        }

        for (size_t vi : c.vars)
            if (!vars[vi].done) {
                std::string msg = "controller '" + c.name + "' variable '" +
                                  vars[vi].name + "': could not evaluate '" +
                                  vars[vi].text + "'";
                if (!vars[vi].error.empty()) msg += " -- " + vars[vi].error;
                add_problem(problems, msg);
            }

        for (CtrlControl& cc : c.controls) {
            bool was_expr = false;
            std::string body = strip_expr_wrapper(cc.text, was_expr);
            EvalOutcome r = eval_expression(body, res, global.species);
            if (r.ok) {
                cc.ok = true;
                cc.value = r.value;
                set_scalar_num(t, cc.expr_node, r.value);
                // Control expressions are delayed (s:controller): kept so they
                // are evaluated again after `post_expansion`.
                if (delayed)
                    delayed->push_back({cc.expr_node, "expression", cc.text,
                                        format_double(r.value)});
            } else if (!r.deferred) {
                std::string msg = "controller '" + c.name +
                                  "' control expression could not be "
                                  "evaluated: " + body;
                if (!r.error.empty()) msg += " -- " + r.error;
                add_problem(problems, msg);
            }
            // Deferred (random()) expressions keep their text, as everywhere
            // else, and so drive nothing: the expanded tree stays reproducible.

            if (cc.target_ctrl == SIZE_MAX) continue;
            // Only ABSOLUTE control reaches the variable now. A RELATIVE
            // controller describes how the variable *changes* once the program
            // varies the knob, which is outside lattice expansion.
            if (!c.absolute || !cc.ok) continue;
            CtrlVar& tv = vars[cs.var_of[{cc.target_ctrl, cc.target_var}]];
            if (!tv.driven) {
                tv.driven = true;
                tv.driven_value = 0.0;
            }
            tv.driven_value += cc.value;  // several ABSOLUTE controllers sum
        }
    }
}

// Gather what drives each lattice parameter. Targets are keyed on the element
// and the path rather than on a parameter node, so a parameter the element does
// not carry yet is still recognised as the same target. Walked in evaluation
// order, and each `controls` entry numbered, so that "previous" has a meaning
// when two of them name one interdependent family.
static TargetMap gather_targets(const ryml::Tree& t, size_t lat_node,
                                const Controllers& cs,
                                const std::vector<size_t>& order,
                                ProblemList& problems) {
    TargetMap targets;
    size_t seq = 0;
    for (size_t ci : order) {
        const Ctrl& c = cs.ctrls[ci];
        auto fail = [&](const std::string& why) {
            add_problem(problems, "controller '" + c.name + "': " + why);
        };
        for (const CtrlControl& cc : c.controls) {
            ++seq;
            if (cc.target_ctrl != SIZE_MAX) continue;
            ElementMatches m = match_element_parameters(t, lat_node, cc.param);
            if (!m.valid) {
                fail("malformed parameter target '" + cc.param + "'");
                continue;
            }
            bool named = m.has_param;
            for (const std::string& p : m.path)
                if (p.empty()) named = false;
            if (!named) {
                fail("target '" + cc.param + "' names no element parameter");
                continue;
            }
            if (m.elements.empty()) {
                fail("target '" + cc.param +
                     "' matches nothing in the expanded lattice");
                continue;
            }
            std::string path;
            for (const std::string& p : m.path)
                path += (path.empty() ? "" : ".") + p;
            for (size_t def : m.elements) {
                CtrlTarget& tg = targets[{def, path}];
                tg.seq = seq;
                tg.expr_nodes.push_back(cc.expr_node);
                if (c.absolute) {
                    tg.has_absolute = true;
                    if (cc.ok)
                        tg.sum += cc.value;
                    else
                        tg.deferred = true;
                } else {
                    tg.has_relative = true;
                }
            }
        }
    }
    return targets;
}

// Apply the ABSOLUTE controllers to the lattice parameters they drive.
//
// Applied in the order the controls were reached, not in the map's key order:
// a later write nullifies the family members an earlier one stated, so which
// one comes last decides what the parameter ends up saying.
static void apply_targets(ryml::Tree& t, const TargetMap& targets,
                          Provenance& prov, ProblemList& problems) {
    std::vector<TargetMap::const_iterator> in_order;
    in_order.reserve(targets.size());
    for (auto it = targets.begin(); it != targets.end(); ++it)
        in_order.push_back(it);
    std::stable_sort(in_order.begin(), in_order.end(),
                     [](const auto& a, const auto& b) {
                         return a->second.seq < b->second.seq;
                     });

    for (const auto& it : in_order) {
        size_t def = it->first.first;
        const CtrlTarget& tg = it->second;
        std::vector<std::string> path = split_dots(it->first.second);
        std::string where = key_str(t, def) + ">" + it->first.second;

        // "A given lattice parameter may not be assigned a delayed evaluation
        // expression and be controlled by a controller." This runs before
        // substitute_values, while the `expr(...)` wrapper is still there to see.
        size_t existing = resolve_param_path(t, def, path);
        if (existing != ryml::NONE && t.has_val(existing)) {
            bool was_expr = false;
            strip_expr_wrapper(val_str(t, existing), was_expr);
            if (was_expr)
                add_problem(problems, where +
                                          " is both controlled and assigned a "
                                          "delayed evaluation expression");
        }

        if (tg.has_absolute && tg.has_relative) {
            add_problem(problems, where +
                                      " is controlled by both an ABSOLUTE and a "
                                      "RELATIVE controller");
            continue;
        }
        // A RELATIVE controller leaves the parameter at the value the element
        // itself gives; only the sum of the ABSOLUTE ones is applied here.
        if (!tg.has_absolute || tg.deferred) continue;

        size_t parent = def;
        for (size_t i = 0; i + 1 < path.size(); ++i)
            parent = find_or_add_map_child(t, parent, path[i].c_str());
        set_num_child(t, parent, path.back().c_str(), tg.sum);
        // The value is the controllers' now, not any single text's: its link
        // is dropped, and the controllers are its drivers.
        link_to_writer(t.find_child(parent, ryml::to_csubstr(path.back())),
                       ryml::NONE, prov);
        // The controller states this member of its family; what the element
        // definition, or a control reached earlier, said through another member
        // is nullified rather than left to be flagged as inconsistent.
        nullify_family(t, def, path, prov);
    }
}

// Record every controller driving a parameter, ABSOLUTE or RELATIVE -- several
// may, so this is a list rather than a link. Done once all the writes are made,
// and by path, so a node one of them removed is not left behind.
static void record_drivers(const ryml::Tree& t, const TargetMap& targets,
                           const Provenance& prov,
                           std::map<size_t, std::vector<size_t>>& drivers) {
    drivers.clear();
    for (const auto& kv : targets) {
        size_t param =
            resolve_param_path(t, kv.first.first, split_dots(kv.first.second));
        if (param == ryml::NONE) continue;
        std::vector<size_t>& d = drivers[param];
        for (size_t en : kv.second.expr_nodes) {
            auto p = prov.find(en);
            if (p != prov.end()) d.push_back(p->second);
        }
    }
}

// Evaluates the controllers and applies them to the expanded lattice.
//
// Every control `expression` is computed with its controller's symbol table and
// the number written back into the control entry, and the ABSOLUTE controllers
// are applied to the lattice parameters they drive. `lat_node` is the expanded
// lattice; parameter targets are matched inside it, which is what keeps a
// controller from also driving the unexpanded element definitions still sitting
// in `facility`. Passing ryml::NONE evaluates the controllers without applying
// them.
void evaluate_controllers(ryml::Tree& t, size_t lat_node,
                          const EvalContext& global, Provenance& prov,
                          ProblemList& problems,
                          std::vector<DelayedValue>* delayed,
                          std::map<size_t, std::vector<size_t>>* drivers) {
    Controllers cs = read_controllers(t, problems);
    if (cs.ctrls.empty()) return;

    std::vector<size_t> order = controller_order(cs.ctrls, problems);
    evaluate_in_order(t, cs, order, global, problems, delayed);
    if (lat_node == ryml::NONE) return;

    TargetMap targets = gather_targets(t, lat_node, cs, order, problems);
    apply_targets(t, targets, prov, problems);
    if (drivers) record_drivers(t, targets, prov, *drivers);
}

}  // namespace pals
