// The PALS lattice expansion pipeline's entry points: parse_and_expand_PALS and
// expand_PALS_string, which build the five-tree representation (original /
// combined / expanded / full_expanded / adjunct), and the C API that links
// nodes across those trees. The stages the work tree is taken through are
// declared in pals_pipeline.h, each in a file of its own; this file runs them
// in order and cuts the results apart. Name matching and parameter lookup live
// in pals_match.cpp; the generic YAML tree wrapper in PALSParserCpp.cpp.

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "PALSParserCpp.h"
#include "pals_check.h"
#include "pals_expression.h"
#include "pals_pipeline.h"
#include "pals_problem.h"
#include "pals_util.h"
#include "yaml_tree.h"

namespace pals {

// ---------------------------------------------------------------------------
// The `expanded` tree: the finished lattice minus everything the bookkeeper
// computed.
//
// What counts as computed is recorded as the set of *authored* parameter paths,
// taken before any bookkeeping runs, rather than as a list of what the
// bookkeeper writes. Node ids cannot serve: a post-expansion `set` nullifies a
// parameter family with t.remove(), ryml recycles the freed ids, and a recorded
// id would then name a different node -- the hazard nullify_family already
// guards provenance against. And recording writes would be wrong even so:
// set_num_child overwrites an existing key in place, so write_ref writes an
// author's `E_tot_ref` without creating it, and it would be lost.
//
// A path is `branch-index/line-index/key` for an element's own parameter and
// `branch-index/line-index/group/key` for one inside a parameter group, which is
// as deep as a PALS element nests. Positions rather than names because names
// repeat: one definition used three times gives three elements called `q1`.
// Positions survive bookkeeping -- forks append their branches during expand(),
// before the snapshot, and the only element the bookkeeper adds is the
// `branch_end` it appends after the last one of a branch.

// The authored shape of a lattice: which elements it held, and which parameters
// each of those carried.
struct AuthoredParams {
    std::set<std::string> elements;  // "branch/line"
    std::set<std::string> params;    // "branch/line[/group]/key"
};

static std::string ele_path(size_t branch, size_t line) {
    return std::to_string(branch) + "/" + std::to_string(line);
}

// Record the lattice's authored shape. Run after expansion and expression
// evaluation but before the bookkeeper, so what it sees is the author's inputs,
// resolved: substituted, unrolled, evaluated, and driven by any ABSOLUTE
// controller.
static void collect_authored(ryml::Tree& t, size_t lat_node,
                             AuthoredParams& out) {
    for_each_line_element(
        t, lat_node, [&](size_t b, size_t l, size_t ele, size_t) {
            const std::string ep = ele_path(b, l);
            out.elements.insert(ep);
            for (size_t c = t.first_child(ele); c != ryml::NONE;
                 c = t.next_sibling(c)) {
                if (!t.has_key(c)) continue;
                const std::string key = key_str(t, c);
                if (!t.is_map(c)) {
                    // A scalar (`length`) or a sequence (`ForkFromP`), held or
                    // dropped whole.
                    out.params.insert(ep + "/" + key);
                    continue;
                }
                // The group is recorded in its own right as well as by its
                // members: writing the group is meaningful even when it holds
                // nothing the author set, since presence is what makes the
                // bookkeeper materialize its defaults.
                out.params.insert(ep + "/" + key);
                for (size_t g = t.first_child(c); g != ryml::NONE;
                     g = t.next_sibling(g))
                    if (t.has_key(g))
                        out.params.insert(ep + "/" + key + "/" +
                                          key_str(t, g));
            }
        });
}

// Fold the parameters a post-expansion `set` wrote into the authored set. Those
// sets run after the first bookkeeper pass, so the snapshot taken before it has
// not seen them -- without this, `expanded` would drop the very value the set
// wrote and keep what the second pass derived from it. The targets name their
// element by node id, which is still live (nothing removes an element), so the
// branch/line position is read back off the tree.
static void add_set_targets(ryml::Tree& t, size_t lat_node,
                            const std::vector<SetTarget>& writes,
                            AuthoredParams& out) {
    if (writes.empty()) return;

    std::map<size_t, std::string> where;
    for_each_line_element(
        t, lat_node, [&](size_t b, size_t l, size_t ele, size_t) {
            where[ele] = ele_path(b, l);
        });

    for (const SetTarget& tg : writes) {
        auto it = where.find(tg.ele);
        if (it == where.end() || tg.path.empty()) continue;
        // `path` is the parameter's route within the element: [key] for one of
        // the element's own, [group, key] for one inside a group.
        std::string p = it->second;
        for (const std::string& step : tg.path) p += "/" + step;
        out.params.insert(p);
        if (tg.path.size() >= 2) out.params.insert(it->second + "/" + tg.path[0]);
    }
}

// Cut every computed parameter out of a copy of the finished lattice, leaving
// the authored inputs. Nodes are gathered first and removed afterwards: removal
// frees ids, which would invalidate the walk that is still using them.
static void prune_computed_params(ryml::Tree& t, size_t lat_node,
                                  const AuthoredParams& authored,
                                  Provenance& prov) {
    std::vector<size_t> drop;

    for_each_line_element(
        t, lat_node, [&](size_t b, size_t l, size_t ele, size_t entry) {
            const std::string ep = ele_path(b, l);
            if (!authored.elements.count(ep)) {
                // An element the bookkeeper added: the `branch_end` Placeholder
                // capping the branch, which exists only to carry the final
                // reference and floor. Drop the seq entry rather than just the
                // definition, so the line is not left holding an empty slot.
                drop.push_back(entry);
                return;
            }
            for (size_t c = t.first_child(ele); c != ryml::NONE;
                 c = t.next_sibling(c)) {
                if (!t.has_key(c)) continue;
                const std::string key = key_str(t, c);
                // `kind` says what the element is rather than how it is set up;
                // it is held whatever the snapshot saw.
                if (key == "kind") continue;

                const std::string path = ep + "/" + key;
                if (!t.is_map(c)) {
                    if (!authored.params.count(path)) drop.push_back(c);
                    continue;
                }

                std::vector<size_t> stale;
                bool any_authored = false;
                for (size_t g = t.first_child(c); g != ryml::NONE;
                     g = t.next_sibling(g)) {
                    if (!t.has_key(g)) continue;
                    if (authored.params.count(
                            path + "/" +
                            key_str(t, g)))
                        any_authored = true;
                    else
                        stale.push_back(g);
                }
                // A group with nothing authored left in it goes too -- unless
                // the author wrote the group itself, in which case it stays,
                // empty, as written.
                if (!any_authored && !authored.params.count(path))
                    drop.push_back(c);
                else
                    drop.insert(drop.end(), stale.begin(), stale.end());
            }
        });

    for (size_t n : drop) {
        remove_tracked(t, n, prov);
    }
}

// Rewrite the `destination_pointer` scalars of a freshly split-out tree.
// handle_fork stores the raw node id of the fork's destination element, but it
// runs while expansion is still on the work tree; cutting the lattice out into
// its own tree renumbers every node, so each pointer has to be translated to
// the id its target now carries. `from_work` maps work ids to ids in `t`. A
// pointer whose target did not come across (it should always be inside the
// lattice) is left alone and reported.
static void remap_destination_pointers(ryml::Tree& t, size_t node,
                                       const Provenance& from_work,
                                       ProblemList& problems) {
    if (node == ryml::NONE) return;

    if (t.has_key(node) &&
        t.key(node) == ryml::to_csubstr("destination_pointer") &&
        t.has_val(node)) {
        size_t target = 0;
        if (!parse_node_id(val_str(t, node), target)) return;
        auto it = from_work.find(target);
        if (it == from_work.end()) {
            add_problem(
                problems,
                "destination_pointer target is outside the expanded lattice");
            return;
        }
        t.set_val(node, t.to_arena(ryml::to_csubstr(std::to_string(it->second))));
    }

    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        remap_destination_pointers(t, c, from_work, problems);
}

// Copy the lattice out of the work tree into a tree of its own: a map holding
// the single `name: {...}` entry. The root is synthesised (a ryml root cannot
// itself carry a key), so it has no counterpart in combined and no provenance
// entry. When there is no lattice the tree stays an empty map.
static ParsedData* split_out_lattice(
    ryml::Tree& t, size_t lat_node, const Provenance& work_prov,
    const std::map<size_t, std::vector<size_t>>& work_drivers,
    ProblemList& problems) {
    ParsedData* out = new ParsedData();
    out->tree.reserve(out->tree.capacity() + t.capacity() + 16);
    out->tree.reserve_arena(out->tree.arena_capacity() + t.arena_capacity());
    out->tree.ref(out->tree.root_id()) |= ryml::MAP;
    if (lat_node == ryml::NONE) return out;

    ensure_capacity(out->tree);
    size_t entry = out->tree.append_child(out->tree.root_id());
    deep_copy_tracked(out->tree, entry, t, lat_node, out->provenance);

    // Before provenance is chained up to combined it still reads
    // this-tree->work, which inverts into exactly the renaming the fork
    // pointers need.
    Provenance from_work;
    for (const auto& kv : out->provenance) from_work[kv.second] = kv.first;
    remap_destination_pointers(out->tree, out->tree.root_id(), from_work,
                               problems);

    // The drivers, re-keyed from work nodes to this tree's.
    for (const auto& kv : out->provenance) {
        auto d = work_drivers.find(kv.second);
        if (d != work_drivers.end()) out->drivers[kv.first] = d->second;
    }

    chain_prov(out->provenance, work_prov);
    return out;
}

// The `post_expansion` list (s:lattice.expand), acting on the lattice just
// built: its sets reach each expanded copy of an element separately. The
// delayed expressions are then evaluated again, against the lattice the sets
// left, and the controllers applied again (over both lists) so they stay
// authoritative over any parameter a set touched. The bookkeeper runs a second
// time to recompute what those writes invalidated -- reference and floor
// parameters, and the family members a set dropped.
static void run_post_expansion(ParsedData& work, size_t lat_node,
                               const std::vector<size_t>& post,
                               const std::vector<DelayedValue>& delayed,
                               AuthoredParams& authored,
                               ProblemList& problems) {
    ryml::Tree& t = work.tree;
    std::vector<SetTarget> set_writes;
    run_post_expansion_sets(t, lat_node, post, work.provenance, problems,
                            &set_writes);
    std::set<size_t> set_nodes;
    for (const SetTarget& w : set_writes) {
        size_t n = resolve_param_path(t, w.ele, w.path);
        if (n != ryml::NONE) set_nodes.insert(n);
    }
    std::vector<DelayedValue> back = restore_delayed(t, delayed, set_nodes);
    // These writes are inputs like any other, but they land after the snapshot
    // of the authored inputs; join them to it before the second bookkeeper pass
    // derives their families again.
    add_set_targets(t, lat_node, set_writes, authored);
    evaluate_expressions(t, lat_node, work.provenance, problems, nullptr,
                         &work.drivers);
    refresh_changed_delayed(t, back, work.provenance);
    run_element_bookkeeper(t, lat_node, problems);
}

// Expand the root lattice `lat_node` of the work tree and fill in everything
// the finished lattice carries, recording in `authored` what the author wrote.
static void build_lattice(ParsedData& work, size_t lat_node,
                          std::map<std::string, size_t>& emap,
                          const std::vector<size_t>& post,
                          AuthoredParams& authored, ProblemList& problems) {
    ryml::Tree& t = work.tree;
    expand_lattice(t, lat_node, emap, work.provenance, problems);

    // Evaluate every mathematical expression to a number (immediate and
    // expr()-delayed alike), and apply the ABSOLUTE controllers to the
    // parameters they drive -- both before the bookkeeper, so the reference and
    // dependent parameters below are computed from the driven values. Node ids
    // of existing nodes are unchanged -- only scalar text is rewritten -- so
    // provenance stays valid; a parameter a controller creates is new and
    // simply has no provenance, like ReferenceP/FloorP.
    std::vector<DelayedValue> delayed;
    evaluate_expressions(t, lat_node, work.provenance, problems, &delayed,
                         &work.drivers);

    // The last point at which the lattice holds the author's inputs and nothing
    // else. Record their paths now; everything the finished lattice carries
    // that is not among them was computed, and is what tells `expanded` apart
    // from `full_expanded`.
    collect_authored(t, lat_node, authored);

    // With every input now a plain number, walk each branch element-by-element
    // to fill in the reference, floor, s-position, and field-dependent output
    // parameters. This adds new ReferenceP/FloorP nodes, which carry no
    // provenance (like destination_pointer) and so simply do not appear in the
    // correspondence map.
    run_element_bookkeeper(t, lat_node, problems);

    if (!post.empty())
        run_post_expansion(work, lat_node, post, delayed, authored, problems);

    // Last, so the line indices it records are the finished ones -- the
    // bookkeeper caps every branch with a `branch_end` element.
    link_fork_connections(t, lat_node);
}

// Builds the `expanded`, `full_expanded` and `adjunct` trees from `combined`.
//
// Expansion has to run on the whole document at once — the lattice pulls in
// element and beamline definitions from the rest of the file — so it happens on
// a single throwaway work tree, which is then cut up: the root lattice is taken
// twice, once whole as `full_expanded` and once pruned back to the author's
// inputs as `expanded`, and `adjunct` takes everything the lattice left behind.
// All three record provenance straight back to `combined`, so the work tree can
// be discarded.
static void make_expanded_and_adjunct(ParsedData* comb,
                                       const char* root_lattice,
                                       ProblemList& problems,
                                       YAMLTreeHandle& expanded_out,
                                       YAMLTreeHandle& full_expanded_out,
                                       YAMLTreeHandle& adjunct_out) {
    expanded_out = nullptr;
    full_expanded_out = nullptr;
    adjunct_out = nullptr;
    if (!comb) return;

    ParsedData work;
    ryml::Tree& t = work.tree;
    t.reserve(t.capacity() + comb->tree.capacity() + 10000);
    t.reserve_arena(t.arena_capacity() + comb->tree.arena_capacity() + 100000);

    // Start from a full copy of combined, recording work->combined provenance
    // for every node.
    deep_copy_tracked(t, t.root_id(), comb->tree, comb->tree.root_id(),
                      work.provenance);

    // The `facility` list is walked first, in order: its sets act on the
    // element definitions, so expansion copies the values they write into every
    // use of a definition, and its immediate expressions and inherits are
    // settled at their place in the list.
    check_definitions(t, problems);
    run_facility_list(t, pals_list_entries(t, "facility"), work.provenance,
                      problems);
    const std::vector<size_t> post = pals_list_entries(t, "post_expansion");

    std::map<std::string, size_t> emap;
    make_ele_map(emap, t, t.root_id());

    std::string name_str = root_lattice ? root_lattice : "";
    size_t lat_node = find_lattice(t, name_str);

    // `skip` is the node the adjunct tree must not copy: the lattice, together
    // with the facility list entry wrapping it, so adjunct is not left holding
    // an empty entry. A lattice that is not a lone entry under a wrapper (e.g.
    // one keyed directly into a map) is skipped on its own.
    size_t skip = ryml::NONE;

    // What the author asked for, against which the bookkeeper's output is told
    // apart further down; filled in just before the bookkeeper runs.
    AuthoredParams authored;

    if (lat_node == ryml::NONE) {
        add_problem(problems, name_str.empty()
                                  ? "no lattice found to expand"
                                  : "lattice '" + name_str + "' not found");
    } else {
        build_lattice(work, lat_node, emap, post, authored, problems);

        size_t wrapper = t.parent(lat_node);
        skip = (wrapper != ryml::NONE && !t.is_root(wrapper) &&
                t.num_children(wrapper) == 1)
                   ? wrapper
                   : lat_node;
    }

    // full_expanded: the lattice as the bookkeeper left it, everything computed.
    ParsedData* full = split_out_lattice(t, lat_node, work.provenance,
                                         work.drivers, problems);

    // expanded: the same lattice, pruned back to what the author wrote. It is
    // cut from the same work tree rather than from `full`, so the two are
    // independent -- freeing one leaves the other whole. Its faults are `full`'s
    // faults and have just been reported, so the second split's problems are
    // dropped rather than said twice.
    ProblemList said_already;
    ParsedData* mini =
        split_out_lattice(t, lat_node, work.provenance, work.drivers,
                          said_already);
    if (lat_node != ryml::NONE) {
        size_t entry = mini->tree.first_child(mini->tree.root_id());
        prune_computed_params(mini->tree, entry, authored, mini->provenance);
    }

    // adjunct: the whole document minus what went to the expanded trees.
    ParsedData* left = new ParsedData();
    left->tree.reserve(left->tree.capacity() + t.capacity() + 16);
    left->tree.reserve_arena(left->tree.arena_capacity() + t.arena_capacity());
    deep_copy_tracked_except(left->tree, left->tree.root_id(), t, t.root_id(),
                             skip, left->provenance);
    chain_prov(left->provenance, work.provenance);

    expanded_out = mini;
    full_expanded_out = full;
    adjunct_out = left;
}

// The path an in-memory document is keyed by in `original`. A string has no
// directory of its own, so a relative `include` or `load` inside one resolves
// against the current directory; a document that names other files is better
// read with parse_and_expand_PALS, which resolves them against itself.
static const char* STRING_DOC_PATH = "<string>";

// An owning C copy of `s`, for handing a std::string across the C API. Always
// returns a string, empty rather than null, so a caller reading `path` never
// has to null-check before comparing.
static char* dup_cstr(const std::string& s) {
    char* out = new char[s.size() + 1];
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

// The whole of parse_and_expand_PALS below the reading of the top-level
// document: `src` is that document parsed (null if it could not be), `top_path`
// the path it is keyed by, and `source_name` what a parse failure calls it.
static struct lattices expand_document(ParsedData* src,
                                       const std::string& top_path,
                                       const std::string& source_name,
                                       const char* root_lattice) {
    struct lattices lat = {};
    ProblemList problems;
    // Built as a derivation chain so provenance can be recorded at each step:
    //   original --(splice includes, merge loads)--> combined
    //   combined  --(expand, split)--> expanded, full_expanded, adjunct
    std::string parse_error;
    lat.original = make_original(src, top_path, parse_error);
    if (!parse_error.empty()) {
        // The top-level document is not valid YAML: there is nothing to expand.
        // Free the empty stand-in, leave all five handles NULL, and report the
        // location as the single problem so the caller can pinpoint the fault.
        delete_tree(lat.original);
        lat.original = nullptr;
        add_problem(problems,
                    "could not parse '" + source_name + "': " + parse_error,
                    source_name);
    } else {
        lat.combined = make_combined_from_original(
            static_cast<ParsedData*>(lat.original), top_path, problems);

        // Spell-check against combined, not expanded: every definition appears
        // there exactly once, as written, so each misspelling is reported once
        // and names the element the user typed rather than one of the copies
        // expansion made of it.
        check_pals_names(GET_TREE(lat.combined), problems);

        make_expanded_and_adjunct(static_cast<ParsedData*>(lat.combined),
                                   root_lattice, problems, lat.expanded,
                                   lat.full_expanded, lat.adjunct);
    }

    // Hand the problem list to the caller as an owning C array (freed with
    // free_lattice_problems). The library never prints — the caller decides
    // whether to report, save, or ignore.
    lat.problems.count = problems.size();
    lat.problems.items =
        problems.empty() ? nullptr : new struct problem[problems.size()];
    for (size_t i = 0; i < problems.size(); ++i) {
        lat.problems.items[i].message = dup_cstr(problems[i].message);
        lat.problems.items[i].path = dup_cstr(problems[i].path);
        lat.problems.items[i].severity =
            problems[i].severity == Severity::Warning ? PROBLEM_WARNING
                                                      : PROBLEM_ERROR;
        switch (problems[i].origin) {
            case Origin::Unsupported:
                lat.problems.items[i].origin = PROBLEM_UNSUPPORTED;
                break;
            case Origin::Unspecified:
                lat.problems.items[i].origin = PROBLEM_UNSPECIFIED;
                break;
            case Origin::Input:
                lat.problems.items[i].origin = PROBLEM_INPUT;
                break;
        }
    }
    return lat;
}

}  // namespace pals

using pals::expand_document;
using pals::fold_path;
using pals::STRING_DOC_PATH;

extern "C" {

YAML_API struct lattices parse_and_expand_PALS(const char* filename,
                                      const char* root_lattice) {
    // Every file is keyed in `original` by its folded path, so folding the
    // top-level one here is what makes it agree with the paths the files
    // themselves resolve to.
    const std::string top_path = fold_path(filename ? filename : "");
    ParsedData* src = static_cast<ParsedData*>(parse_file(top_path.c_str()));
    return expand_document(src, top_path, filename ? filename : "",
                           root_lattice);
}

YAML_API struct lattices expand_PALS_string(const char* yaml_str,
                                            const char* root_lattice) {
    return expand_document(static_cast<ParsedData*>(parse_string(yaml_str)),
                           STRING_DOC_PATH, STRING_DOC_PATH, root_lattice);
}

YAML_API void free_lattice_problems(struct problem_list problems) {
    for (size_t i = 0; i < problems.count; ++i) {
        delete[] problems.items[i].message;
        delete[] problems.items[i].path;
    }
    delete[] problems.items;
}

YAML_API double evaluate_pals_expression(const char* expr, bool* ok) {
    if (ok) *ok = false;
    if (!expr) return 0.0;
    bool was_expr = false;
    std::string body = strip_expr_wrapper(expr, was_expr);
    pals::EvalOutcome r = pals::eval_expression(body, pals::SymbolLookup{});
    if (!r.ok) return 0.0;
    if (ok) *ok = true;
    return r.value;
}

YAML_API struct correspondence_map build_correspondence_map(
    YAMLTreeHandle original, YAMLTreeHandle combined,
    YAMLTreeHandle full_expanded, YAMLTreeHandle adjunct) {
    (void)original;  // provenance is in combined, full_expanded & adjunct
    struct correspondence_map out = {nullptr, 0};
    if (!combined || (!full_expanded && !adjunct)) return out;

    ParsedData* comb = static_cast<ParsedData*>(combined);
    const std::map<size_t, size_t>& c2o = comb->provenance;  // combined->original

    std::vector<struct node_link> links;

    // Emit one link per node of a derived tree, walking it from the root so that
    // exactly the live nodes are visited. Expansion splits the document in two,
    // so a link names a node in one derived tree and YAML_NULL_ID in the other;
    // the shared combined id is what ties the two sides together.
    auto walk = [&](YAMLTreeHandle handle, bool is_adjunct) {
        if (!handle) return;
        ParsedData* pd = static_cast<ParsedData*>(handle);
        const std::map<size_t, size_t>& d2c = pd->provenance;  // derived->combined
        ryml::Tree& dt = pd->tree;

        std::vector<size_t> stack;
        if (dt.root_id() != ryml::NONE) stack.push_back(dt.root_id());
        while (!stack.empty()) {
            size_t n = stack.back();
            stack.pop_back();

            struct node_link link;
            link.full_expanded = is_adjunct ? YAML_NULL_ID : n;
            link.adjunct = is_adjunct ? n : YAML_NULL_ID;
            auto dc = d2c.find(n);
            if (dc != d2c.end()) {
                link.combined = dc->second;
                auto co = c2o.find(dc->second);
                link.original = (co != c2o.end()) ? co->second : YAML_NULL_ID;
            } else {
                link.combined = YAML_NULL_ID;
                link.original = YAML_NULL_ID;
            }
            links.push_back(link);

            for (size_t c = dt.first_child(n); c != ryml::NONE;
                 c = dt.next_sibling(c))
                stack.push_back(c);
        }
    };

    walk(full_expanded, false);
    walk(adjunct, true);

    out.count = links.size();
    if (out.count > 0) {
        out.links = new struct node_link[out.count];
        std::copy(links.begin(), links.end(), out.links);
    }
    return out;
}

YAML_API YAMLNodeId original_node(YAMLTreeHandle combined, YAMLTreeHandle tree,
                                  YAMLNodeId node) {
    if (!combined || !tree || node == YAML_NULL_ID) return YAML_NULL_ID;
    const std::map<size_t, size_t>& c2o =
        static_cast<ParsedData*>(combined)->provenance;
    size_t c = node;
    if (tree != combined) {
        const std::map<size_t, size_t>& d2c =
            static_cast<ParsedData*>(tree)->provenance;
        auto dc = d2c.find(node);
        if (dc == d2c.end()) return YAML_NULL_ID;
        c = dc->second;
    }
    auto co = c2o.find(c);
    return co == c2o.end() ? YAML_NULL_ID : co->second;
}

YAML_API struct node_list controller_expressions(YAMLTreeHandle combined,
                                                 YAMLTreeHandle tree,
                                                 YAMLNodeId node) {
    struct node_list out = {nullptr, 0};
    if (!combined || !tree || node == YAML_NULL_ID) return out;
    const std::map<size_t, size_t>& c2o =
        static_cast<ParsedData*>(combined)->provenance;
    const auto& drivers = static_cast<ParsedData*>(tree)->drivers;
    auto d = drivers.find(node);
    if (d == drivers.end()) return out;
    std::vector<YAMLNodeId> ids;
    for (size_t c : d->second) {
        auto co = c2o.find(c);
        if (co != c2o.end()) ids.push_back(co->second);
    }
    out.count = ids.size();
    if (out.count) {
        out.nodes = new YAMLNodeId[out.count];
        std::copy(ids.begin(), ids.end(), out.nodes);
    }
    return out;
}

YAML_API void free_node_list(struct node_list list) { delete[] list.nodes; }

YAML_API void free_correspondence_map(struct correspondence_map map) {
    delete[] map.links;
}

}  // extern "C"
