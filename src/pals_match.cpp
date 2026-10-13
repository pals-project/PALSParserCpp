// PALS name matching and parameter lookup: resolves a PALS name-matching
// string (see match_names in PALSParserCpp.h) against a tree using PCRE2, and
// reads the value of a matched element parameter, constant, or variable. The
// lattice expansion that produces the trees this runs on lives in
// pals_expand.cpp; the generic YAML tree wrapper in PALSParserCpp.cpp.

#include "PALSParserCpp.h"
#include "yaml_tree.h"
#include "pals_util.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <set>
#include <string>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// ============================================================
// NAME MATCHING
// ============================================================
//
// Implements PALS name matching (names.md). A match string selects a set of
// named constructs -- elements, element parameter groups, element parameters,
// controllers, controller variables, constants, and variables. The full form is
//
//   {element-set}[>{group}.{sub}. ... .{param}]
//
// where {element-set} is a `,` union of `&` intersections of `:` ranges of
// element names, and an element name is
//
//   [{lattice}>>>][{branch}>>][{kind}::]{name}[#{N}]
//
// The operators bind, tightest first: `>>>`, `>>`, `::`, `#`, `:`, `&`, `,`,
// `>`. {lattice}, {branch} and {name} are PCRE2 patterns matched against the
// whole name; anything inside a `(...)`, `[...]` or `{...}` group belongs to the
// pattern, so `Q\d{2,3}` is one name and not a union. A {name} made only of
// digits is the 1-based index of an element in its line. {kind} is an element
// kind, `Controller`, or a MetaP string component (`alias::black` matches the
// elements whose MetaP.alias is `black`). The parameter path after the single
// `>` is matched exactly, key by key, and applies to every element of the set.
//
// What an unqualified name reaches depends on the tree. In an expanded lattice
// it reaches the elements of the lattice. Before expansion -- a tree holding a
// `PALS` node -- it reaches the element definitions standing under `facility`
// (or `post_expansion`), not the elements written inside a beamline's `line`;
// those answer only to a name qualified by the beamline that holds them.

// Compile a PCRE2 pattern. Returns nullptr on error (message to stderr).
static pcre2_code* compile_pattern(const std::string& pat) {
    int errnum = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code* re =
        pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pat.c_str()), pat.size(), 0,
                      &errnum, &erroff, nullptr);
    if (!re) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(errnum, buf, sizeof(buf));
        std::cerr << "match_names: bad regex '" << pat << "' at offset "
                  << erroff << ": " << buf << "\n";
    }
    return re;
}

// A compiled pattern, shared so that the selector holding it can be copied.
// Null means "match any" (an omitted component).
using Pattern = std::shared_ptr<pcre2_code>;

static Pattern make_pattern(const std::string& pat, bool& ok) {
    if (pat.empty()) return nullptr;
    pcre2_code* re = compile_pattern(pat);
    if (!re) {
        ok = false;
        return nullptr;
    }
    return Pattern(re, pcre2_code_free);
}

// Whole-string (anchored at both ends) match of `subject` against a compiled
// pattern. A null pattern means "match anything" (an omitted component).
static bool full_match(const Pattern& re, const std::string& subject) {
    if (!re) return true;
    pcre2_match_data* md =
        pcre2_match_data_create_from_pattern(re.get(), nullptr);
    int rc = pcre2_match(re.get(), reinterpret_cast<PCRE2_SPTR>(subject.c_str()),
                         subject.size(), 0,
                         PCRE2_ANCHORED | PCRE2_ENDANCHORED, md, nullptr);
    pcre2_match_data_free(md);
    return rc >= 0;
}

static std::string node_key_str(const ryml::Tree& t, size_t n) {
    if (n == ryml::NONE || !t.has_key(n)) return "";
    return std::string(t.key(n).str, t.key(n).len);
}

static std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The positions in `s` that lie outside every bracket group, i.e. where an
// operator character means the operator. A backslash escapes the character
// after it, as it does in the pattern.
static std::vector<bool> top_level_mask(const std::string& s) {
    std::vector<bool> top(s.size(), false);
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\') {
            ++i;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
            continue;
        }
        if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            --depth;
            continue;
        }
        top[i] = depth == 0;
    }
    return top;
}

// True if the run of `c` characters through position i is exactly `n` long.
static bool run_of(const std::string& s, size_t i, char c, size_t n) {
    size_t a = i, b = i;
    while (a > 0 && s[a - 1] == c) --a;
    while (b < s.size() && s[b] == c) ++b;
    return b - a == n;
}

// Split `s` at the top-level occurrences of the one-character operator `op`
// that stand alone (a `:` that is not part of `::`, a `>` that is not part of
// `>>`).
static std::vector<std::string> split_top(const std::string& s, char op) {
    std::vector<bool> top = top_level_mask(s);
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!top[i] || s[i] != op || !run_of(s, i, op, 1)) continue;
        out.push_back(s.substr(from, i - from));
        from = i + 1;
    }
    out.push_back(s.substr(from));
    return out;
}

// The first top-level occurrence of the run `op` (`>>>`, `>>`, `::`) standing
// exactly as long as `op`, or npos.
static size_t find_top(const std::string& s, const std::string& op) {
    std::vector<bool> top = top_level_mask(s);
    for (size_t i = 0; i + op.size() <= s.size(); ++i) {
        if (!top[i] || s.compare(i, op.size(), op) != 0) continue;
        if (!run_of(s, i, op[0], op.size())) continue;
        return i;
    }
    return std::string::npos;
}

// The MetaP components whose value an element can be matched by.
static bool is_meta_key(const std::string& k) {
    return k == "alias" || k == "ID" || k == "label" || k == "description";
}

// One element name: [{lattice}>>>][{branch}>>][{kind}::]{name}[#{N}].
struct EleName {
    Pattern lattice, branch, name;
    bool lattice_present = false;
    bool branch_present = false;
    std::string kind;     // element kind or `Controller`; "" for none
    std::string meta;     // MetaP component when the prefix names one
    long index = 0;       // {name} is an element index (1-based), 0 if not
    long instance = 0;    // #N, 0 if not given
    std::string raw_name; // {name} as written
    bool plain() const {  // a bare name: no qualifier of any sort
        return !lattice_present && !branch_present && kind.empty() &&
               meta.empty() && instance == 0;
    }
};

// A `:` range, or a lone element name when `to` is absent.
struct RangeTerm {
    EleName from;
    EleName to;
    bool is_range = false;
};

struct Selector {
    // The element set: a union of intersections of ranges.
    std::vector<std::vector<RangeTerm>> sets;
    bool has_param = false;
    std::vector<std::string> path;
    bool valid = true;
    // True when the set is a single bare name, which also names constants,
    // variables and controllers.
    bool single_plain() const {
        return sets.size() == 1 && sets[0].size() == 1 &&
               !sets[0][0].is_range && sets[0][0].from.plain();
    }
};

static EleName parse_ele_name(const std::string& text, bool& ok) {
    EleName e;
    std::string s = trimmed(text);
    if (s.empty()) {
        ok = false;
        return e;
    }
    std::string lattice, branch;
    size_t p3 = find_top(s, ">>>");
    if (p3 != std::string::npos) {
        lattice = trimmed(s.substr(0, p3));
        s = trimmed(s.substr(p3 + 3));
        e.lattice_present = true;
    }
    size_t p2 = find_top(s, ">>");
    if (p2 != std::string::npos) {
        branch = trimmed(s.substr(0, p2));
        s = trimmed(s.substr(p2 + 2));
        e.branch_present = true;
    }
    size_t pk = find_top(s, "::");
    if (pk != std::string::npos) {
        std::string k = trimmed(s.substr(0, pk));
        s = trimmed(s.substr(pk + 2));
        if (is_meta_key(k))
            e.meta = k;
        else
            e.kind = k;
    }
    std::vector<bool> top = top_level_mask(s);
    for (size_t i = s.size(); i-- > 0;) {
        if (!top[i] || s[i] != '#') continue;
        std::string n = trimmed(s.substr(i + 1));
        char* end = nullptr;
        long v = std::strtol(n.c_str(), &end, 10);
        if (n.empty() || *end != '\0' || v < 1) {
            ok = false;
            return e;
        }
        e.instance = v;
        s = trimmed(s.substr(0, i));
        break;
    }
    e.raw_name = s;
    if (!s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
            return c >= '0' && c <= '9';
        })) {
        e.index = std::strtol(s.c_str(), nullptr, 10);
        if (e.index < 1) ok = false;
    } else {
        e.name = make_pattern(s, ok);
    }
    e.lattice = make_pattern(lattice, ok);
    e.branch = make_pattern(branch, ok);
    return e;
}

// Parse a match string into a Selector, compiling its patterns.
static Selector parse_selector(const std::string& spec) {
    Selector sel;
    bool ok = true;
    std::string set = spec;
    std::vector<std::string> parts = split_top(spec, '>');
    if (parts.size() > 2) {
        sel.valid = false;
        return sel;
    }
    if (parts.size() == 2) {
        set = parts[0];
        std::string path = trimmed(parts[1]);
        sel.path = split_dots(path);
        sel.has_param = true;
        for (const std::string& p : sel.path)
            if (p.empty()) ok = false;
    }
    for (const std::string& u : split_top(set, ',')) {
        std::vector<RangeTerm> inter;
        for (const std::string& a : split_top(u, '&')) {
            std::vector<std::string> ends = split_top(a, ':');
            if (ends.size() > 2) {
                ok = false;
                continue;
            }
            RangeTerm r;
            r.from = parse_ele_name(ends[0], ok);
            if (ends.size() == 2) {
                r.to = parse_ele_name(ends[1], ok);
                r.is_range = true;
                // A range lies within one line: the end is searched for in the
                // line the start is in, so it takes the start's qualifiers.
                if (!r.to.lattice_present && !r.to.branch_present) {
                    r.to.lattice = r.from.lattice;
                    r.to.branch = r.from.branch;
                    r.to.lattice_present = r.from.lattice_present;
                    r.to.branch_present = r.from.branch_present;
                }
            }
            inter.push_back(std::move(r));
        }
        sel.sets.push_back(std::move(inter));
    }
    sel.valid = ok;
    return sel;
}

// ------------------------------------------------------------
// The population a selector is run against.
// ------------------------------------------------------------

// One element sitting in a line, or one standalone definition.
struct Candidate {
    size_t node = ryml::NONE;   // keyed definition map, or a bare line entry
    std::string name;
    size_t line = SIZE_MAX;     // index into Population::lines; SIZE_MAX = none
    size_t pos = 0;             // 0-based position in that line
    std::string lattice;        // enclosing Lattice name
    std::vector<std::string> beamlines;  // enclosing branch / BeamLine names
};

struct Population {
    // Elements reached through a line, grouped per line in line order. An
    // element may sit in more than one line (a sub-line's elements are also
    // elements of the line that uses it), and so appear more than once.
    std::vector<std::vector<Candidate>> lines;
    // Elements an unqualified name reaches directly.
    std::vector<Candidate> defs;
    // Whether an unqualified name also reaches the line elements: true in an
    // expanded lattice, false before expansion.
    bool unqualified_reaches_lines = true;
};

// The kind of a candidate. A bare line entry carries none of its own.
static std::string cand_kind(const ryml::Tree& t, const Candidate& c) {
    return t.is_map(c.node) ? child_val_str(t, c.node, "kind") : "";
}

static bool name_ok(const ryml::Tree& t, const EleName& e, const Candidate& c,
                    bool in_line) {
    if (e.lattice_present && (!in_line || !full_match(e.lattice, c.lattice)))
        return false;
    if (e.branch_present) {
        if (!in_line) return false;
        bool any = false;
        for (const std::string& b : c.beamlines)
            if (full_match(e.branch, b)) {
                any = true;
                break;
            }
        if (!any) return false;
    }
    if (e.index > 0) {
        if (!in_line || c.pos + 1 != static_cast<size_t>(e.index)) return false;
    } else if (e.meta.empty()) {
        if (!full_match(e.name, c.name)) return false;
    }
    if (!e.kind.empty() && cand_kind(t, c) != e.kind) return false;
    if (!e.meta.empty()) {
        if (!t.is_map(c.node)) return false;
        size_t meta = t.find_child(c.node, ryml::to_csubstr("MetaP"));
        if (meta == ryml::NONE || !t.is_map(meta)) return false;
        size_t v = t.find_child(meta, ryml::to_csubstr(e.meta));
        if (v == ryml::NONE || !t.has_val(v)) return false;
        std::string val = trimmed(std::string(t.val(v).str, t.val(v).len));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
            val.back() == val.front())
            val = val.substr(1, val.size() - 2);
        if (!full_match(e.name, val)) return false;
    }
    return true;
}

// Every candidate an element name selects, in population order, with `#N`
// applied last.
static std::vector<const Candidate*> eval_name(const ryml::Tree& t,
                                               const Population& pop,
                                               const EleName& e) {
    std::vector<const Candidate*> out;
    // Line elements first, then standalone definitions, each in order.
    bool qualified = e.lattice_present || e.branch_present || e.index > 0;
    if (qualified || pop.unqualified_reaches_lines)
        for (const auto& line : pop.lines)
            for (const Candidate& c : line)
                if (name_ok(t, e, c, true)) out.push_back(&c);
    if (!qualified)
        for (const Candidate& c : pop.defs)
            if (name_ok(t, e, c, false)) out.push_back(&c);
    // One element reached through several lines is one match.
    std::vector<const Candidate*> uniq;
    std::set<size_t> seen;
    for (const Candidate* c : out)
        if (seen.insert(c->node).second) uniq.push_back(c);
    if (e.instance > 0) {
        if (static_cast<size_t>(e.instance) > uniq.size()) return {};
        return {uniq[e.instance - 1]};
    }
    return uniq;
}

// The nodes a range term selects, in order.
static std::vector<size_t> eval_range(const ryml::Tree& t, const Population& pop,
                                      const RangeTerm& r) {
    std::vector<size_t> out;
    std::vector<const Candidate*> from = eval_name(t, pop, r.from);
    if (!r.is_range) {
        for (const Candidate* c : from) out.push_back(c->node);
        return out;
    }
    std::vector<const Candidate*> to = eval_name(t, pop, r.to);
    std::set<size_t> to_nodes;
    for (const Candidate* c : to) to_nodes.insert(c->node);
    for (const Candidate* a : from) {
        if (a->line == SIZE_MAX) continue;  // a definition sits in no line
        const auto& line = pop.lines[a->line];
        // The first end element at or after the start, wrapping around.
        size_t n = line.size();
        for (size_t k = 0; k < n; ++k) {
            size_t i = (a->pos + k) % n;
            if (!to_nodes.count(line[i].node)) continue;
            for (size_t j = 0; j <= k; ++j)
                out.push_back(line[(a->pos + j) % n].node);
            break;
        }
    }
    return out;
}

// The element set a selector names, in order and without repeats.
static std::vector<size_t> eval_sets(const ryml::Tree& t, const Population& pop,
                                     const Selector& sel) {
    std::vector<size_t> out;
    std::set<size_t> seen;
    for (const auto& inter : sel.sets) {
        std::vector<size_t> acc;
        for (size_t i = 0; i < inter.size(); ++i) {
            std::vector<size_t> nodes = eval_range(t, pop, inter[i]);
            if (i == 0) {
                acc = nodes;
                continue;
            }
            std::set<size_t> keep(nodes.begin(), nodes.end());
            std::vector<size_t> next;
            for (size_t n : acc)
                if (keep.count(n)) next.push_back(n);
            acc.swap(next);
        }
        for (size_t n : acc)
            if (seen.insert(n).second) out.push_back(n);
    }
    return out;
}

// True if the definition `def` is a lattice element (or a bare line reference),
// as opposed to a beamline, lattice, controller, constant or variable.
static bool is_element_def(const ryml::Tree& t, size_t def) {
    if (!t.is_map(def)) return true;
    std::string k = child_val_str(t, def, "kind");
    return k != "BeamLine" && k != "Lattice" && k != "Controller" &&
           k != "constant" && k != "variable";
}

// Add the elements of a `line` sequence, one Population line per beamline. In
// an expanded lattice the lines are already flat; before expansion an entry may
// name another BeamLine definition (found in `beamline_defs`), whose elements
// are then elements of this line as well -- `B1>>Saf` reaches an `Saf` in a
// sub-line of B1. `active` guards against a line that contains itself.
static void add_line(const ryml::Tree& t, size_t line,
                     const std::string& lattice,
                     const std::vector<std::string>& beamlines,
                     const std::map<std::string, size_t>* beamline_defs,
                     std::set<std::string>& active,
                     std::vector<Candidate>& cur) {
    for (size_t entry = t.first_child(line); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        Candidate c;
        c.lattice = lattice;
        c.beamlines = beamlines;
        if (t.is_map(entry)) {
            size_t def = t.first_child(entry);
            if (def == ryml::NONE || !t.has_key(def)) continue;
            c.node = def;
            c.name = node_key_str(t, def);
        } else if (t.has_val(entry)) {
            c.node = entry;
            c.name = std::string(t.val(entry).str, t.val(entry).len);
        } else {
            continue;
        }
        if (beamline_defs) {
            // A sub-line reference contributes its elements in place.
            auto it = beamline_defs->find(c.name);
            bool own_kind = t.is_map(c.node) &&
                            !child_val_str(t, c.node, "kind").empty();
            if (it != beamline_defs->end() && !own_kind &&
                !active.count(c.name)) {
                size_t sub = t.find_child(it->second, ryml::to_csubstr("line"));
                if (sub != ryml::NONE && t.is_seq(sub)) {
                    active.insert(c.name);
                    std::vector<std::string> chain = beamlines;
                    chain.push_back(c.name);
                    add_line(t, sub, lattice, chain, beamline_defs, active,
                             cur);
                    active.erase(c.name);
                    continue;
                }
            }
        }
        cur.push_back(std::move(c));
    }
}

static void finish_line(Population& pop, std::vector<Candidate>&& cur) {
    size_t li = pop.lines.size();
    for (size_t i = 0; i < cur.size(); ++i) {
        cur[i].line = li;
        cur[i].pos = i;
    }
    pop.lines.push_back(std::move(cur));
}

// The expanded-lattice population below `root`: every map owning a `line`
// sequence is a branch, and its key the branch name.
static void collect_expanded(const ryml::Tree& t, size_t node,
                             const std::string& lattice, Population& pop) {
    if (node == ryml::NONE) return;
    std::string cur_lattice = lattice;
    if (t.is_map(node) && child_val_str(t, node, "kind") == "Lattice")
        cur_lattice = node_key_str(t, node);
    size_t line = t.is_map(node) ? t.find_child(node, ryml::to_csubstr("line"))
                                 : ryml::NONE;
    if (line != ryml::NONE && t.is_seq(line)) {
        std::vector<Candidate> cur;
        std::set<std::string> active;
        add_line(t, line, cur_lattice, {node_key_str(t, node)}, nullptr, active,
                 cur);
        finish_line(pop, std::move(cur));
        return;
    }
    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        collect_expanded(t, c, cur_lattice, pop);
}

// The pre-expansion population of a list of `facility`/`post_expansion`
// entries: their element definitions, reached by unqualified names, and the
// lines of their BeamLines, reached by beamline-qualified names.
static void collect_definitions(const ryml::Tree& t,
                                const std::vector<size_t>& entries,
                                Population& pop) {
    pop.unqualified_reaches_lines = false;
    std::map<std::string, size_t> beamlines;
    std::vector<size_t> defs;
    for (size_t entry : entries) {
        if (entry == ryml::NONE || !t.is_map(entry)) continue;
        size_t def = t.first_child(entry);
        if (def == ryml::NONE || !t.has_key(def) || !t.is_map(def)) continue;
        defs.push_back(def);
        if (child_val_str(t, def, "kind") == "BeamLine")
            beamlines.emplace(node_key_str(t, def), def);
    }
    for (size_t def : defs) {
        if (is_element_def(t, def)) {
            Candidate c;
            c.node = def;
            c.name = node_key_str(t, def);
            pop.defs.push_back(std::move(c));
        }
        size_t line = t.find_child(def, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;
        std::vector<Candidate> cur;
        std::set<std::string> active{node_key_str(t, def)};
        add_line(t, line, "", {node_key_str(t, def)}, &beamlines, active, cur);
        finish_line(pop, std::move(cur));
    }
}

// The entries of every `facility` and `post_expansion` list in the tree. There
// is one of each per PALS node, and the PALS node sits at the root in most
// views but one level down per file in `original`, so the whole tree is walked.
static void definition_entries(const ryml::Tree& t, size_t node,
                               std::vector<size_t>& out) {
    if (node == ryml::NONE) return;
    std::string k = node_key_str(t, node);
    if (t.is_seq(node) && (k == "facility" || k == "post_expansion")) {
        for (size_t e = t.first_child(node); e != ryml::NONE;
             e = t.next_sibling(e))
            out.push_back(e);
        return;
    }
    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        definition_entries(t, c, out);
}

// True if the tree is a pre-expansion view: it holds a PALS node, at the root
// or one level down (per file, in `original`).
static bool holds_pals(const ryml::Tree& t) {
    size_t root = t.root_id();
    if (root == ryml::NONE || !t.is_map(root)) return false;
    if (t.find_child(root, ryml::to_csubstr("PALS")) != ryml::NONE) return true;
    for (size_t c = t.first_child(root); c != ryml::NONE; c = t.next_sibling(c))
        if (t.is_map(c) && t.find_child(c, ryml::to_csubstr("PALS")) != ryml::NONE)
            return true;
    return false;
}

// The population of a whole tree, as match_names sees it.
static Population tree_population(const ryml::Tree& t) {
    Population pop;
    if (holds_pals(t)) {
        std::vector<size_t> entries;
        definition_entries(t, t.root_id(), entries);
        collect_definitions(t, entries, pop);
    } else {
        collect_expanded(t, t.root_id(), "", pop);
    }
    return pop;
}

// Every `kind: Controller` map in the tree whose name matches `re`.
static void match_controllers(const ryml::Tree& t, size_t node,
                              const Pattern& re, std::vector<size_t>& out) {
    if (node == ryml::NONE) return;
    if (t.is_map(node) && t.has_key(node) &&
        child_val_str(t, node, "kind") == "Controller") {
        if (full_match(re, node_key_str(t, node))) out.push_back(node);
        return;
    }
    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        match_controllers(t, c, re, out);
}

// The controllers a selector names: `Controller::{name}` terms, and -- for a
// single bare name -- any controller of that name.
static std::vector<size_t> selector_controllers(const ryml::Tree& t,
                                                const Selector& sel) {
    std::vector<size_t> out;
    for (const auto& inter : sel.sets) {
        if (inter.size() != 1 || inter[0].is_range) continue;
        const EleName& e = inter[0].from;
        if (e.kind == "Controller" && !e.lattice_present && !e.branch_present &&
            e.index == 0 && e.meta.empty())
            match_controllers(t, t.root_id(), e.name, out);
    }
    if (sel.single_plain() && sel.sets[0][0].from.index == 0)
        match_controllers(t, t.root_id(), sel.sets[0][0].from.name, out);
    std::vector<size_t> uniq;
    std::set<size_t> seen;
    for (size_t n : out)
        if (seen.insert(n).second) uniq.push_back(n);
    return uniq;
}

// The variable `name` of a controller, in either the map or the compact
// sequence form of its `variables`; NONE if it has none of that name.
static size_t controller_variable(const ryml::Tree& t, size_t ctrl,
                                  const std::string& name) {
    size_t vars = t.find_child(ctrl, ryml::to_csubstr("variables"));
    if (vars == ryml::NONE) return ryml::NONE;
    if (t.is_map(vars)) return t.find_child(vars, ryml::to_csubstr(name));
    for (size_t e = t.first_child(vars); e != ryml::NONE; e = t.next_sibling(e))
        if (t.is_map(e)) {
            size_t v = t.find_child(e, ryml::to_csubstr(name));
            if (v != ryml::NONE) return v;
        }
    return ryml::NONE;
}

// Heap-copy a std::string into a null-terminated C string the caller frees with
// yaml_free_string() (i.e. allocated with new[], matching that free).
static char* new_c_string(const std::string& s) {
    char* result = new char[s.size() + 1];
    std::memcpy(result, s.c_str(), s.size() + 1);
    return result;
}

// Turn a raw scalar value into a param_value, returning it as stored — the value
// is NOT evaluated. A plain numeric literal (the whole string, bar surrounding
// whitespace, is a finite number) becomes a number; anything else — an
// expression like "0.3 * 5", a species name like "#3He", or any other text — is
// returned verbatim as a string. Evaluation is the job of lattice expansion or
// evaluate_pals_expression, not of this accessor: on the `expanded` tree values
// are already numbers, while the raw views keep their expressions.
static struct param_value value_from_raw(const std::string& raw) {
    struct param_value v = {PARAM_VALUE_STRING, 0.0, nullptr};
    const char* s = raw.c_str();
    char* end = nullptr;
    double d = std::strtod(s, &end);  // skips leading whitespace itself
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') ++end;
    if (end != s && *end == '\0' && std::isfinite(d)) {
        v.kind = PARAM_VALUE_NUMBER;
        v.number = d;
    } else {
        v.string = new_c_string(raw);
    }
    return v;
}

// Resolve one element's parameter into a param_value. `ele` is the element
// definition map; `path` the dotted parameter path walked from it. A parameter
// that is not present yields the default (numeric 0 for now); a scalar value is
// returned as stored (see value_from_raw); a path that stops on a whole
// parameter group rather than a single value is not a parameter value and yields
// PARAM_VALUE_MISSING.
static struct param_value param_value_for_element(
    const ryml::Tree& t, size_t ele, const std::vector<std::string>& path) {
    size_t node = resolve_param_path(t, ele, path);
    if (node == ryml::NONE) {  // element found, parameter unset -> default
        struct param_value v = {PARAM_VALUE_NUMBER, 0.0, nullptr};
        return v;
    }
    if (!t.has_val(node)) {  // resolved to a group/sequence, not a value
        struct param_value v = {PARAM_VALUE_MISSING, 0.0, nullptr};
        return v;
    }
    return value_from_raw(std::string(t.val(node).str, t.val(node).len));
}

// The value of an already-matched node, for a bare-name lookup that resolves to
// a constant or variable. A compact-form entry (`name: value`) is itself the
// scalar; a full-form entry (a map with `kind: constant|variable`) carries its
// value under a `value:` child. Any other node — an element definition or a
// parameter group — has no single scalar value and yields PARAM_VALUE_MISSING.
static struct param_value value_of_const_var(const ryml::Tree& t, size_t node) {
    struct param_value v = {PARAM_VALUE_MISSING, 0.0, nullptr};
    if (node == ryml::NONE) return v;
    if (t.has_val(node))  // compact form: the matched node is the value scalar
        return value_from_raw(std::string(t.val(node).str, t.val(node).len));
    if (t.is_map(node)) {
        std::string kind = child_val_str(t, node, "kind");
        if (kind == "constant" || kind == "variable") {
            size_t val = t.find_child(node, ryml::to_csubstr("value"));
            if (val != ryml::NONE && t.has_val(val))
                return value_from_raw(std::string(t.val(val).str, t.val(val).len));
        }
    }
    return v;
}

// Whether two param_values carry the same value. Used to collapse several
// matches into one result: identical values agree, conflicting values leave the
// parameter unidentified.
static bool param_value_eq(const struct param_value& a,
                           const struct param_value& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == PARAM_VALUE_NUMBER) return a.number == b.number;
    if (a.kind == PARAM_VALUE_STRING) return std::strcmp(a.string, b.string) == 0;
    return true;  // both PARAM_VALUE_MISSING
}

// Reduce a list of per-match param_values to a single result: the shared value
// when they all agree (the same element reused, or several that all default),
// or PARAM_VALUE_MISSING when any two conflict. Consumes `vs`, freeing every
// owned string except the one handed back.
static struct param_value reduce_param_values(std::vector<struct param_value>& vs) {
    struct param_value missing = {PARAM_VALUE_MISSING, 0.0, nullptr};
    if (vs.empty()) return missing;
    bool conflict = false;
    for (size_t i = 1; i < vs.size(); ++i)
        if (!param_value_eq(vs[0], vs[i])) { conflict = true; break; }
    if (conflict) {
        for (struct param_value& v : vs)
            if (v.string) yaml_free_string(v.string);
        return missing;
    }
    for (size_t i = 1; i < vs.size(); ++i)  // keep vs[0]'s string, free the rest
        if (vs[i].string) yaml_free_string(vs[i].string);
    return vs[0];
}

// Classify a single keyed node as a constant/variable and, if its name matches,
// record it. Handles both the full form (a map with kind: constant|variable)
// and the compact form (a `constants:`/`variables:` sequence of name: value).
static void collect_const_var(const ryml::Tree& t, size_t named,
                              const Pattern& name_re, std::vector<size_t>& out,
                              std::set<size_t>& seen) {
    if (named == ryml::NONE || !t.has_key(named)) return;
    std::string key = node_key_str(t, named);
    if (key == "constants" || key == "variables") {
        // Compact form. The list of `name: value` pairs may be written either as
        // a YAML sequence (each entry a single-key map) or as a plain map.
        for (size_t e = t.first_child(named); e != ryml::NONE;
             e = t.next_sibling(e)) {
            size_t item = t.has_key(e) ? e : t.first_child(e);
            if (item != ryml::NONE && t.has_key(item) &&
                full_match(name_re, node_key_str(t, item)) &&
                seen.insert(item).second)
                out.push_back(item);
        }
        return;
    }
    if (t.is_map(named)) {
        std::string kind = child_val_str(t, named, "kind");
        if ((kind == "constant" || kind == "variable") &&
            full_match(name_re, key) && seen.insert(named).second)
            out.push_back(named);
    }
}

// Match a constant/variable name pattern against every constant and variable
// defined directly under a single PALS node (or its facility / post_expansion
// sub-node).
static void match_const_var_in_pals(const ryml::Tree& t, size_t pals,
                                    const Pattern& name_re,
                                    std::vector<size_t>& out,
                                    std::set<size_t>& seen) {
    if (pals == ryml::NONE) return;
    for (size_t c = t.first_child(pals); c != ryml::NONE;
         c = t.next_sibling(c)) {
        std::string k = node_key_str(t, c);
        if (k == "facility" || k == "post_expansion") {
            // A sequence of single-key wrapper maps.
            for (size_t w = t.first_child(c); w != ryml::NONE;
                 w = t.next_sibling(w))
                collect_const_var(t, t.is_map(w) ? t.first_child(w) : ryml::NONE,
                                  name_re, out, seen);
        } else {
            collect_const_var(t, c, name_re, out, seen);
        }
    }
}

// Match constant/variable names across the tree. The PALS node sits at the root
// in the combined and adjunct views; in the `original` view it is nested one
// level down under a per-file wrapper keyed by filename, and there may be
// several. Look in both places so a constant is found in whichever tree
// actually holds it.
static void match_const_var(const ryml::Tree& t, const Pattern& name_re,
                            std::vector<size_t>& out, std::set<size_t>& seen) {
    size_t root = t.root_id();
    if (root == ryml::NONE || !t.is_map(root)) return;

    match_const_var_in_pals(t, t.find_child(root, ryml::to_csubstr("PALS")),
                            name_re, out, seen);
    for (size_t c = t.first_child(root); c != ryml::NONE; c = t.next_sibling(c))
        if (t.is_map(c))
            match_const_var_in_pals(
                t, t.find_child(c, ryml::to_csubstr("PALS")), name_re, out, seen);
}

ElementMatches match_element_parameters(const ryml::Tree& t, size_t root,
                                        const std::string& spec,
                                        const std::vector<size_t>& extra) {
    ElementMatches out;
    Selector sel = parse_selector(spec);
    out.valid = sel.valid;
    out.has_param = sel.has_param;
    out.path = sel.path;
    if (sel.valid) {
        Population pop;
        collect_expanded(t, root == ryml::NONE ? t.root_id() : root, "", pop);
        for (size_t n : extra) {
            Candidate c;
            c.node = n;
            c.name = node_key_str(t, n);
            pop.defs.push_back(std::move(c));
        }
        out.elements = eval_sets(t, pop, sel);
    }
    return out;
}

ElementMatches match_definition_parameters(const ryml::Tree& t,
                                           const std::vector<size_t>& entries,
                                           const std::string& spec) {
    ElementMatches out;
    Selector sel = parse_selector(spec);
    out.valid = sel.valid;
    out.has_param = sel.has_param;
    out.path = sel.path;
    if (out.valid) {
        // A `{lattice}>>>` qualifier cannot apply: no lattice has been built
        // yet, so nothing belongs to one.
        for (const auto& inter : sel.sets)
            for (const RangeTerm& r : inter)
                if (r.from.lattice_present || r.to.lattice_present)
                    out.valid = false;
    }
    if (out.valid) {
        Population pop;
        collect_definitions(t, entries, pop);
        out.elements = eval_sets(t, pop, sel);
    }
    return out;
}

extern "C" {

YAML_API struct name_matches match_names(YAMLTreeHandle tree,
                                          const char* match_string) {
    struct name_matches out = {nullptr, 0};
    if (!tree || !match_string) return out;
    ryml::Tree& t = GET_TREE(tree);

    Selector sel = parse_selector(std::string(match_string));
    std::vector<size_t> hits;
    std::set<size_t> seen;
    auto add = [&](size_t n) {
        if (n != ryml::NONE && seen.insert(n).second) hits.push_back(n);
    };

    if (sel.valid) {
        Population pop = tree_population(t);
        for (size_t ele : eval_sets(t, pop, sel))
            add(sel.has_param ? resolve_param_path(t, ele, sel.path) : ele);

        // Controllers, and through them their variables (`ps27>cur1`).
        for (size_t ctrl : selector_controllers(t, sel)) {
            if (!sel.has_param)
                add(ctrl);
            else if (sel.path.size() == 1)
                add(controller_variable(t, ctrl, sel.path[0]));
            else
                add(resolve_param_path(t, ctrl, sel.path));
        }

        // A bare name also matches constants and variables directly by name.
        if (sel.single_plain() && !sel.has_param &&
            sel.sets[0][0].from.index == 0)
            match_const_var(t, sel.sets[0][0].from.name, hits, seen);
    }

    out.count = hits.size();
    if (out.count > 0) {
        out.nodes = new YAMLNodeId[out.count];
        std::copy(hits.begin(), hits.end(), out.nodes);
    }
    return out;
}

YAML_API void free_name_matches(struct name_matches matches) {
    delete[] matches.nodes;
}

YAML_API struct param_value get_parameter_value(YAMLTreeHandle tree,
                                                const char* match_string) {
    struct param_value out = {PARAM_VALUE_MISSING, 0.0, nullptr};
    if (!tree || !match_string) return out;
    ryml::Tree& t = GET_TREE(tree);

    Selector sel = parse_selector(std::string(match_string));
    if (!sel.valid) return out;  // a bad pattern identifies nothing

    std::vector<struct param_value> values;

    if (sel.has_param) {
        // An element parameter. Find the matching element(s), then walk the
        // path down from each, so an element that exists but does not set the
        // parameter still yields its default.
        Population pop = tree_population(t);
        for (size_t ele : eval_sets(t, pop, sel))
            values.push_back(param_value_for_element(t, ele, sel.path));
        // A controller variable.
        if (sel.path.size() == 1)
            for (size_t ctrl : selector_controllers(t, sel)) {
                size_t v = controller_variable(t, ctrl, sel.path[0]);
                if (v != ryml::NONE && t.has_val(v))
                    values.push_back(value_from_raw(
                        std::string(t.val(v).str, t.val(v).len)));
            }
    } else if (sel.single_plain() && sel.sets[0][0].from.index == 0) {
        // No parameter path: a bare name resolves to a constant or variable.
        // An element has no scalar value, so anything else stays missing.
        std::vector<size_t> nodes;
        std::set<size_t> seen;
        match_const_var(t, sel.sets[0][0].from.name, nodes, seen);
        for (size_t node : nodes)
            values.push_back(value_of_const_var(t, node));
    }

    return reduce_param_values(values);
}

YAML_API struct param_value get_lattice_parameter_value(
    YAMLTreeHandle full_expanded, YAMLTreeHandle adjunct,
    const char* match_string) {
    // Element parameters live in the expanded lattice; look there first.
    struct param_value v = get_parameter_value(full_expanded, match_string);
    if (v.kind != PARAM_VALUE_MISSING) return v;
    // Constants, variables and unused definitions live in the facility
    // scaffolding kept by the adjunct tree.
    return get_parameter_value(adjunct, match_string);
}

}  // extern "C"
