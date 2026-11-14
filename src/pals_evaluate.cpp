// Expression evaluation over the PALS trees: the user constants and variables
// (and the checks on them), the controllers, and the pass that replaces every
// expression with its value. The expression grammar itself lives in
// pals_expression.cpp; this layer supplies the symbols and walks the tree. See
// pals_pipeline.h for where this sits in the pipeline.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "pals_pipeline.h"
#include "pals_util.h"
#include "yaml_tree.h"

namespace pals {

// A short "group.param" (or just "param") location for a value node, for use in
// problem messages. Deliberately shallow so the identical parameter reached
// through several expansion copies yields one message.
static std::string short_location(const ryml::Tree& t, size_t node) {
    std::string key = key_str(t, node);
    size_t p = t.parent(node);
    // An entry of a list of single-key maps (`constants: [- a: ...]`) is
    // located by the list it sits in.
    if (p != ryml::NONE && !t.has_key(p) && t.is_map(p)) {
        size_t gp = t.parent(p);
        if (gp != ryml::NONE && t.is_seq(gp) && t.has_key(gp)) p = gp;
    }
    std::string pk = key_str(t, p);
    if (!pk.empty() && !key.empty()) return pk + "." + key;
    return key;
}

// ============================================================
// EXPRESSION EVALUATION
// ============================================================
//
// After the tree is fully expanded, every scalar value that is a PALS
// mathematical expression is replaced with its evaluated number (immediate and
// `expr(...)`-delayed alike, per the standard's evaluation model as applied to
// the expanded tree). Values that are not expressions -- element/line name
// references, `kind:` names, booleans, etc. -- fail to evaluate and are left
// untouched. Expressions containing random()/random_gauss() are deferred
// (kept as text) so the expanded tree stays reproducible.
//
// The math, functions, built-in constants, and particle-data functions
// (mass_of/charge_of/anomalous_moment_of, from AtomicAndPhysicalConstantsCLib)
// live in pals_expression.cpp; this layer supplies user constants/variables and
// walks the tree.

// Keys whose scalar values are names/flags, never expressions. Skipping them
// avoids a stray collision between such a name and a constant (e.g. an element
// literally named `pi`).
//
// `authors`, `notes` and `reminders` are free-form prose (top-level.md,
// s:palsroot), so nothing in them is an expression. Prose that happens to carry
// a `/` (a file path) or parentheses would otherwise be taken for arithmetic by
// looks_like_expression and reported as a broken expression.
//
// `parameter` (an `adjustments` vary/target), `elements` (GirderP) and
// `origin_ele` (CoordinateSetP) are name-matching strings, and
// `origin_ele_ref_pt` an enum.
//
// `destination_pointer` is here for a different reason: it is a node id, and a
// node id is a number, so evaluating it "succeeds" and rewrites it through
// format_double. That is lossless as a number but not as text -- an id of 110
// comes back as `1.1e+02`, the shortest round-tripping form -- and every reader
// of the pointer parses it with std::stoull, which stops at the `.` and yields
// 1. See handle_fork.
static const std::set<std::string>& non_expr_keys() {
    static const std::set<std::string> keys = {
        "kind",       "include",     "use",
        "inherit",    "zero_point",  "to_line",
        "destination_element", "new_branch", "multipass",
        "propagate_reference", "name", "multipass_index",
        "element_index", "destination_pointer", "forked_to",
        "authors",    "notes",       "reminders",
        "parameter",  "elements",    "origin_ele",
        "origin_ele_ref_pt"};
    return keys;
}

// ------------------------------------------------------------
// Interdependent parameters
//
// Some parameters are computed from one another, so writing one leaves the rest
// of its family stale. A magnetic multipole component of order N has four
// interchangeable forms (BnN, BnNL, KnN, KnNL, tied by the element length and
// the reference momentum); an electric one has two; a bend's geometry is tied
// together through BendP.
//
// parameter-values-and-controllers.md (s:set): "For any group of interdependent
// parameters, the set of one member of the group nullifies previous settings of
// all other members of the group." Nullifying is what keeps the family from
// being flagged as inconsistent when the bookkeeper derives it: whoever writes
// last states the quantity, and the earlier statements of it are gone. The rule
// holds for every writer that acts on a parameter from outside the element
// definition -- `set` commands, before and after expansion, and ABSOLUTE
// controllers alike. Two members written inside one element definition are a
// different matter: neither is "previous", so that stays an inconsistency for
// the bookkeeper to report.
// ------------------------------------------------------------

// The family `key` belongs to within `group`, including `key` itself; empty for
// a parameter that stands alone.
//
// This is also what tells a value expression that a parameter is *not yet
// computable* rather than zero: before the bookkeeper has run, an absent
// parameter with a written family member is one whose value is still to be
// derived (lattice-construction.md, s:lattice.expand).
std::set<std::string> linked_family(const std::string& group,
                                    const std::string& key) {
    // `<letter><n|s><digits>[L]`, the shape both multipole groups use.
    auto split_component = [](const std::string& k, const char* letters,
                              std::string& comp) {
        if (k.size() < 3 || !std::strchr(letters, k[0])) return false;
        if (k[1] != 'n' && k[1] != 's') return false;
        size_t i = 2;
        while (i < k.size() && std::isdigit(static_cast<unsigned char>(k[i])))
            ++i;
        if (i == 2) return false;
        if (i != k.size() && !(i + 1 == k.size() && k[i] == 'L')) return false;
        comp = k.substr(1, i - 1);  // e.g. "n1"
        return true;
    };

    std::string comp;
    if (group == "MagneticMultipoleP" && split_component(key, "BK", comp))
        return {"B" + comp, "B" + comp + "L", "K" + comp, "K" + comp + "L"};
    if (group == "ElectricMultipoleP" && split_component(key, "E", comp))
        return {"E" + comp, "E" + comp + "L"};

    static const std::set<std::string> bend = {
        "g_ref",     "radius_ref", "Bn0_ref",
        "angle_ref", "L_chord",    "L_rectangle"};
    if (group == "BendP" && bend.count(key)) return bend;

    return {};
}

// Drop the rest of the family of the parameter `path` names on `ele`, so the
// bookkeeper derives them again from the value just written. Provenance entries
// go with them: ryml reuses freed ids, and a stale link would then point the
// correspondence map at the wrong node.
void nullify_family(ryml::Tree& t, size_t ele,
                    const std::vector<std::string>& path, Provenance& prov) {
    if (ele == ryml::NONE || path.empty()) return;
    const std::string& key = path.back();
    std::string group = path.size() >= 2 ? path[path.size() - 2] : std::string();
    std::set<std::string> family = linked_family(group, key);
    if (family.empty()) return;

    std::vector<std::string> sib = path;
    for (const std::string& member : family) {
        if (member == key) continue;
        sib.back() = member;
        size_t node = resolve_param_path(t, ele, sib);
        if (node == ryml::NONE) continue;
        remove_tracked(t, node, prov);
    }
}

// True if `node` is a map defining a `kind: Controller` element.
bool is_controller(const ryml::Tree& t, size_t node) {
    if (node == ryml::NONE || !t.is_map(node)) return false;
    size_t kind = t.find_child(node, ryml::to_csubstr("kind"));
    if (kind == ryml::NONE || !t.has_val(kind)) return false;
    return t.val(kind) == ryml::to_csubstr("Controller");
}

// True if `node` is a facility `set` or `sets` node. Their `parameter` targets
// are name-matching strings and their `value`s are expressions evaluated once
// per matched element, both handled by the set pass, so the generic pass has to
// leave the whole subtree alone.
static bool is_set_node(const ryml::Tree& t, size_t node) {
    if (node == ryml::NONE || !t.has_key(node)) return false;
    ryml::csubstr k = t.key(node);
    return k == ryml::to_csubstr("set") || k == ryml::to_csubstr("sets");
}

// Collects user constant/variable definitions (name -> defining expression),
// in both the full (`kind: constant`/`value:`) and compact
// (`constants:`/`variables:` lists) forms. First definition of a name wins;
// the standard requires duplicates to share the same value, which
// check_definitions verifies. `all`, when given, receives every definition,
// duplicates included. Controller subtrees are skipped: their variables are
// controller-scoped, not global symbols.
static void collect_defs(
    const ryml::Tree& t, size_t node, std::map<std::string, std::string>& defs,
    std::vector<std::pair<std::string, std::string>>* all = nullptr) {
    if (node == ryml::NONE || is_controller(t, node)) return;

    // Full form: a named map with `kind: constant|variable` and a `value:`.
    if (t.is_map(node) && t.has_key(node)) {
        size_t kind = t.find_child(node, ryml::to_csubstr("kind"));
        if (kind != ryml::NONE && t.has_val(kind)) {
            std::string k = val_str(t, kind);
            if (k == "constant" || k == "variable") {
                size_t val = t.find_child(node, ryml::to_csubstr("value"));
                if (val != ryml::NONE && t.has_val(val)) {
                    std::string name = key_str(t, node);
                    std::string text = val_str(t, val);
                    defs.emplace(name, text);
                    if (all) all->emplace_back(name, text);
                }
            }
        }
    }

    // Compact form: a `constants:`/`variables:` list of single-key maps
    // (`- const_a: ...`). A plain map under the key is not the compact form;
    // check_definitions reports it.
    std::string k = key_str(t, node);
    if ((k == "constants" || k == "variables") && t.is_seq(node)) {
        for_each_pair(t, node, [&](size_t kv) {
            if (!t.has_key(kv) || !t.has_val(kv)) return;
            std::string name = key_str(t, kv);
            std::string text = val_str(t, kv);
            defs.emplace(name, text);
            if (all) all->emplace_back(name, text);
        });
    }

    for (size_t c = t.first_child(node); c != ryml::NONE;
         c = t.next_sibling(c))
        collect_defs(t, c, defs, all);
}

// True if a scalar was *meant* to be an expression — it carries an arithmetic
// operator, grouping, an element-parameter reference (`>`), or an explicit
// `expr(...)` wrapper — as opposed to a plain name, label, or boolean. Used to
// decide whether a value that failed to evaluate is worth reporting: a bare
// word that does not resolve is almost always a name, not a broken expression.
// (`-` is excluded so a hyphenated string is not mistaken for a subtraction.)
static bool looks_like_expression(const std::string& body, bool was_expr) {
    if (was_expr) return true;
    for (char c : body)
        if (c == '+' || c == '*' || c == '/' || c == '^' || c == '(' ||
            c == ')' || c == '>')
            return true;
    return false;
}

// Whether the scalar `node` holds a name, a flag or prose rather than an
// expression. A keyed value is judged by its key. A bare sequence element has no
// key of its own, so the decision belongs to the key of the list it sits in --
// the entries of `notes` are prose for the same reason the key is, and the
// entries of a beamline `line:` are name references.
static bool holds_non_expression(const ryml::Tree& t, size_t node) {
    if (t.has_key(node)) return non_expr_keys().count(key_str(t, node)) != 0;
    size_t p = t.parent(node);
    if (p == ryml::NONE || !t.has_key(p)) return false;
    std::string pk = key_str(t, p);
    return pk == "line" || non_expr_keys().count(pk) != 0;
}

// Evaluate the scalar `node` in place. See substitute_values.
static void substitute_scalar(ryml::Tree& t, size_t node, const EvalContext& ctx,
                              ProblemList& problems,
                              const SubstituteMode& mode) {
    bool was_expr = false;
    std::string text = val_str(t, node);
    std::string body = strip_expr_wrapper(text, was_expr);
    if (was_expr && mode.immediate_only) return;

    EvalOutcome r = eval_expression(body, ctx.resolve, ctx.species);
    std::string sp;
    if (r.ok) {
        std::string num = format_double(r.value);
        t.set_val(node, t.to_arena(ryml::to_csubstr(num)));
        if (was_expr && mode.delayed && t.has_key(node))
            mode.delayed->push_back({node, key_str(t, node), text, num});
    } else if (mode.immediate_only) {
        // Not evaluable yet -- it may need what expansion computes. The pass
        // after expansion evaluates it, or reports it.
    } else if (!r.deferred && ctx.species && ctx.species(body, sp)) {
        // A bare identifier that names a species-valued constant or variable
        // (e.g. `species_ref: species` with `species: "#3He"`) is replaced by
        // its species-name string. Double-quoted so a leading `#` survives
        // YAML's comment rule on re-emit.
        t.set_val(node, t.to_arena(ryml::to_csubstr(sp)));
        t.set_val_style(node, ryml::VAL_DQUO);
    } else if (!r.deferred && looks_like_expression(body, was_expr)) {
        std::string loc = short_location(t, node);
        std::string msg = "could not evaluate expression";
        if (!loc.empty()) msg += " for " + loc;
        msg += ": " + body;
        if (!r.error.empty()) msg += " -- " + r.error;
        add_problem(problems, msg);
    }
}

void substitute_values(ryml::Tree& t, size_t node, const EvalContext& ctx,
                       ProblemList& problems, const SubstituteMode& mode) {
    if (node == ryml::NONE || is_controller(t, node) || is_set_node(t, node))
        return;
    if (t.has_val(node) && !holds_non_expression(t, node))
        substitute_scalar(t, node, ctx, problems, mode);
    for (size_t c = t.first_child(node); c != ryml::NONE;
         c = t.next_sibling(c))
        substitute_values(t, c, ctx, problems, mode);
}

// The identifiers appearing in an expression: maximal runs of name characters
// that do not start with a digit. Used to spot a name an expression is not
// allowed to use, which a plain substring search would get wrong (`c_light`
// does not reference a variable named `light`).
std::vector<std::string> expression_identifiers(const std::string& s) {
    std::vector<std::string> out;
    auto name_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    for (size_t i = 0; i < s.size();) {
        if (!name_char(s[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && name_char(s[j])) ++j;
        if (!std::isdigit(static_cast<unsigned char>(s[i])))
            out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// Resolves an element-parameter reference `element>group.sub. ... .param` to
// the scalar value node it names, or NONE. Only the exact single-element form
// is supported (a value expression references one specific parameter; pattern
// matching and branch/lattice qualifiers are not permitted here, per the
// standard). `emap` maps element names to their definition maps.
static size_t resolve_ele_param_ref(const ryml::Tree& t,
                                    const std::map<std::string, size_t>& emap,
                                    const std::string& ref) {
    size_t gt = ref.find('>');
    if (gt == std::string::npos) return ryml::NONE;
    std::string elem = ref.substr(0, gt);
    std::string rest = ref.substr(gt + 1);
    // Reject empty parts and any remaining '>' (e.g. `branch>>ele>...`).
    if (elem.empty() || rest.empty() || rest.find('>') != std::string::npos)
        return ryml::NONE;
    auto it = emap.find(elem);
    if (it == emap.end()) return ryml::NONE;
    size_t node = resolve_param_path(t, it->second, split_dots(rest));
    if (node == ryml::NONE || !t.has_val(node)) return ryml::NONE;
    return node;
}

// Builds the expression-evaluation context for `t`: a resolver for user
// constants/variables and element-parameter references, and the species-name
// lookup that lets a particle-data function take a symbol. The tables the two
// read are held by shared_ptr, so the returned functions stay usable after this
// returns -- `set` commands need a context per command, since each one runs
// against a tree the previous ones have written to.
EvalContext make_expression_context(const ryml::Tree& t, size_t lat_node) {
    auto defs = std::make_shared<std::map<std::string, std::string>>();
    collect_defs(t, t.root_id(), *defs);

    // Element name -> definition map, so expressions may reference another
    // element's parameter via `element>group. ... .param`. Once the lattice is
    // expanded an unqualified name means an element of it (names.md), so its
    // elements are entered first and win over the facility definitions.
    auto emap = std::make_shared<std::map<std::string, size_t>>();
    if (lat_node != ryml::NONE) make_ele_map(*emap, t, lat_node);
    make_ele_map(*emap, t, t.root_id());

    EvalContext ctx;

    // Resolves a symbol whose value is a species-name string (e.g.
    // `species: "#3He"`), so a particle-data function may take it by name:
    // `mass_of(species)`. The stored value is returned verbatim (trimmed, with
    // any surrounding quotes stripped); the expression evaluator validates it.
    ctx.species = [defs](const std::string& name, std::string& out) -> bool {
        auto di = defs->find(name);
        if (di == defs->end()) return false;
        if (di->second.find_first_not_of(" \t\r\n") == std::string::npos)
            return false;
        out = unquote(trim(di->second));
        return true;
    };

    // Lazily evaluate user symbols on demand, memoizing results and guarding
    // against reference cycles. A symbol whose defining expression is itself
    // unresolvable is reported as unknown.
    //
    // The resolver recurses through itself, so it is held by a shared_ptr that
    // the returned lookup owns, and reached from inside through a weak_ptr: a
    // closure owning a shared_ptr to itself would never be freed.
    auto cache = std::make_shared<std::map<std::string, double>>();
    auto active = std::make_shared<std::set<std::string>>();
    auto resolve = std::make_shared<SymbolLookup>();
    std::weak_ptr<SymbolLookup> self = resolve;
    const ryml::Tree* tp = &t;
    SpeciesLookup species = ctx.species;
    *resolve = [defs, tp, emap, cache, active, self, species](
                   const std::string& name, double& out) -> bool {
        auto ci = cache->find(name);
        if (ci != cache->end()) {
            out = ci->second;
            return true;
        }
        // An element-parameter reference (`element>group. ... .param`) resolves
        // to that parameter's value, evaluated as an expression in turn.
        std::string body;
        if (name.find('>') != std::string::npos) {
            size_t vn = resolve_ele_param_ref(*tp, *emap, name);
            if (vn == ryml::NONE) return false;
            body = val_str(*tp, vn);
        } else {
            auto di = defs->find(name);
            if (di == defs->end()) return false;
            body = di->second;
        }
        if (!active->insert(name).second) return false;  // cycle
        bool was_expr = false;
        body = strip_expr_wrapper(body, was_expr);
        std::shared_ptr<SymbolLookup> me = self.lock();
        EvalOutcome r = eval_expression(body, *me, species);
        active->erase(name);
        if (!r.ok) return false;
        (*cache)[name] = r.value;
        out = r.value;
        return true;
    };
    ctx.resolve = [resolve](const std::string& name, double& out) {
        return (*resolve)(name, out);
    };
    return ctx;
}

// Where a definition may stand and what it may be defined with
// (s:constants): directly under the `facility` or `post_expansion` list, the
// compact form as a list, and a constant never with a delayed expression.
static void check_definition_form(const ryml::Tree& t, size_t node,
                                  ProblemList& problems) {
    if (node == ryml::NONE || is_controller(t, node)) return;
    auto list_key = [&](size_t n) -> std::string {
        // `n` is the content of a list entry: entry map -> sequence -> key.
        size_t entry = t.parent(n);
        if (entry == ryml::NONE || t.has_key(entry)) return "";
        size_t seq = t.parent(entry);
        if (seq == ryml::NONE || !t.is_seq(seq) || !t.has_key(seq)) return "";
        return key_str(t, seq);
    };
    auto placed = [&](size_t n, const std::string& what) {
        std::string lk = list_key(n);
        if (lk == "facility" || lk == "post_expansion") return;
        add_problem(problems,
                    what + " must be defined directly under the `facility` or "
                    "`post_expansion` list", what);
    };
    auto delayed_constant = [&](const std::string& name,
                                const std::string& text) {
        bool was_expr = false;
        strip_expr_wrapper(text, was_expr);
        if (was_expr)
            add_problem(problems,
                        "constant '" + name + "' is defined with a delayed "
                        "expression; only a variable may be", name);
    };

    if (t.has_key(node)) {
        std::string k = key_str(t, node);
        if (k == "constants" || k == "variables") {
            placed(node, "`" + k + "`");
            if (!t.is_seq(node)) {
                add_problem(problems,
                            "`" + k + "` must be a list of `name: value` "
                            "entries", k);
            } else if (k == "constants") {
                for_each_pair(t, node, [&](size_t kv) {
                    if (t.has_key(kv) && t.has_val(kv))
                        delayed_constant(key_str(t, kv), val_str(t, kv));
                });
            }
            return;
        }
        if (t.is_map(node)) {
            std::string kind = child_val_str(t, node, "kind");
            if (kind == "constant" || kind == "variable") {
                placed(node, kind + " '" + k + "'");
                if (kind == "constant")
                    delayed_constant(k, child_val_str(t, node, "value"));
                return;
            }
        }
    }
    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        check_definition_form(t, c, problems);
}

// Nothing in the `facility` list may reference what the `post_expansion` list
// defines (s:lattice.expand): its constants, variables and controllers.
static void check_facility_references(const ryml::Tree& t,
                                      ProblemList& problems) {
    size_t pals = t.find_child(t.root_id(), ryml::to_csubstr("PALS"));
    if (pals == ryml::NONE) return;
    size_t post = t.find_child(pals, ryml::to_csubstr("post_expansion"));
    size_t fac = t.find_child(pals, ryml::to_csubstr("facility"));
    if (post == ryml::NONE || fac == ryml::NONE) return;

    std::map<std::string, std::string> post_defs;
    collect_defs(t, post, post_defs);
    std::set<std::string> names;
    for (const auto& d : post_defs) names.insert(d.first);
    for (size_t e = t.first_child(post); e != ryml::NONE; e = t.next_sibling(e)) {
        size_t c = t.first_child(e);
        if (c != ryml::NONE && t.has_key(c) && is_controller(t, c))
            names.insert(key_str(t, c));
    }
    if (names.empty()) return;

    std::set<std::string> said;
    std::function<void(size_t)> walk = [&](size_t n) {
        if (t.has_val(n)) {
            std::string v = val_str(t, n);
            for (const std::string& id : expression_identifiers(v))
                if (names.count(id) && said.insert(id).second)
                    add_problem(problems,
                                "'" + id + "' is defined in `post_expansion` "
                                "and may not be referenced from `facility`",
                                id);
        }
        for (size_t c = t.first_child(n); c != ryml::NONE; c = t.next_sibling(c))
            walk(c);
    };
    walk(fac);
}

void check_definitions(const ryml::Tree& t, ProblemList& problems) {
    check_definition_form(t, t.root_id(), problems);
    check_facility_references(t, problems);

    std::map<std::string, std::string> defs;
    std::vector<std::pair<std::string, std::string>> all;
    collect_defs(t, t.root_id(), defs, &all);

    // Circular definitions, found by walking the names each definition uses.
    std::map<std::string, int> state;  // 1 = on the walk, 2 = done
    std::set<std::string> reported;
    std::function<void(const std::string&, std::vector<std::string>&)> visit =
        [&](const std::string& name, std::vector<std::string>& stack) {
            state[name] = 1;
            stack.push_back(name);
            bool was_expr = false;
            for (const std::string& id : expression_identifiers(
                     strip_expr_wrapper(defs[name], was_expr))) {
                if (!defs.count(id)) continue;
                if (state[id] == 1) {
                    auto from = std::find(stack.begin(), stack.end(), id);
                    std::string chain;
                    for (auto it = from; it != stack.end(); ++it)
                        chain += *it + " -> ";
                    if (reported.insert(id).second)
                        add_problem(problems,
                                    "circular definition: " + chain + id, id);
                } else if (state[id] == 0) {
                    visit(id, stack);
                }
            }
            stack.pop_back();
            state[name] = 2;
        };
    for (const auto& d : defs) {
        std::vector<std::string> stack;
        if (state[d.first] == 0) visit(d.first, stack);
    }

    // Redefinitions to a different value.
    EvalContext ctx = make_expression_context(t);
    std::set<std::string> said;
    for (const auto& d : all) {
        const std::string& first = defs[d.first];
        if (d.second == first || said.count(d.first)) continue;
        bool w1 = false, w2 = false;
        EvalOutcome a = eval_expression(strip_expr_wrapper(first, w1),
                                        ctx.resolve, ctx.species);
        EvalOutcome b = eval_expression(strip_expr_wrapper(d.second, w2),
                                        ctx.resolve, ctx.species);
        if (a.ok && b.ok && a.value == b.value) continue;
        said.insert(d.first);
        add_problem(problems,
                    "'" + d.first + "' is defined more than once with different "
                    "values ('" + first + "' and '" + d.second + "')",
                    d.first);
    }
}

void evaluate_expressions(ryml::Tree& t, size_t lat_node, Provenance& prov,
                          ProblemList& problems,
                          std::vector<DelayedValue>* delayed,
                          std::map<size_t, std::vector<size_t>>* drivers) {
    EvalContext ctx = make_expression_context(t, lat_node);
    evaluate_controllers(t, lat_node, ctx, prov, problems, delayed, drivers);
    SubstituteMode mode;
    mode.delayed = delayed;
    substitute_values(t, t.root_id(), ctx, problems, mode);
}

// Put back the `expr(...)` text of the delayed values the first evaluation
// replaced, so the next one evaluates them against the lattice as it now stands
// (after `post_expansion`). A value no longer holding the number written over
// it -- removed, or replaced by a `set` -- is left alone: a set replaces a
// delayed expression like any other value (s:set).
std::vector<DelayedValue> restore_delayed(
    ryml::Tree& t, const std::vector<DelayedValue>& d,
    const std::set<size_t>& written) {
    std::vector<DelayedValue> back;
    for (const DelayedValue& v : d) {
        if (written.count(v.node)) continue;
        if (v.node >= t.capacity() || !t.has_key(v.node) || !t.has_val(v.node))
            continue;
        if (key_str(t, v.node) != v.key || val_str(t, v.node) != v.written)
            continue;
        t.set_val(v.node, t.to_arena(ryml::to_csubstr(v.text)));
        back.push_back(v);
    }
    return back;
}

// A parameter group key: CamelCase ending in `P` (lattice-element-parameters.md).
static bool looks_like_group_name(const std::string& k) {
    return k.size() >= 2 && k.back() == 'P' && k[0] >= 'A' && k[0] <= 'Z';
}

// A delayed element parameter that came out different the second time is a new
// statement of its quantity, as a `set` would be: the family members the first
// bookkeeper pass derived from the old value are dropped, to be derived again.
void refresh_changed_delayed(ryml::Tree& t,
                             const std::vector<DelayedValue>& back,
                             Provenance& prov) {
    for (const DelayedValue& v : back) {
        if (!t.has_val(v.node) || val_str(t, v.node) == v.written) continue;
        size_t p = t.parent(v.node);
        if (p == ryml::NONE || !t.has_key(p)) continue;
        std::vector<std::string> path{v.key};
        size_t ele = p;
        std::string pk = key_str(t, p);
        if (looks_like_group_name(pk)) {
            path.insert(path.begin(), pk);
            ele = t.parent(p);
        }
        if (ele == ryml::NONE || !t.has_key(ele)) continue;
        nullify_family(t, ele, path, prov);
    }
}

}  // namespace pals
