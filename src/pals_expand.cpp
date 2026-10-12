// PALS lattice expansion pipeline: builds the four-tree representation
// (original / combined / expanded / adjunct) from a PALS YAML file. Splices
// includes, merges `load`ed files, expands the selected lattice (repeats,
// inherits, forks), evaluates expressions into the expanded tree, and applies
// the controllers that drive
// its parameters. Name matching and parameter lookup live in pals_match.cpp;
// the generic YAML tree wrapper in PALSParserCpp.cpp.

#include "PALSParserCpp.h"
#include "yaml_tree.h"
#include "pals_util.h"
#include "pals_floor.h"
#include "pals_check.h"
#include "pals_problem.h"

#include "apc/apc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pals_expression.h"

// ============================================================
// PROVENANCE-TRACKED COPY HELPERS
// ============================================================
//
// These mirror the plain copy/duplicate routines but record, for every node
// they create, which source node it came from. The recorded map (`prov`) is
// what lets build_correspondence_map() link a node across the four trees.

// Cross-tree deep copy (like deep_copy_recursive) that records
// prov[dst_node] = src_node for every copied node. The destination root keeps
// no key even if src_node has one (a tree root cannot be keyed). `skip` names a
// source node to leave out along with its descendants (ryml::NONE copies
// everything); it is how the adjunct tree is built as the document minus the
// lattice that went into the expanded tree.
static void deep_copy_tracked_except(ryml::Tree& dst_t, size_t dst_node,
                                     const ryml::Tree& src_t, size_t src_node,
                                     size_t skip,
                                     std::map<size_t, size_t>& prov) {
    prov[dst_node] = src_node;

    // Copy type flags (MAP, SEQ, VAL, KEY). A tree root cannot carry a key, so
    // drop the KEY flag when the destination is the root.
    auto src_type = src_t.type(src_node);
    dst_t.ref(dst_node) |= (src_type & (ryml::MAP | ryml::SEQ | ryml::VAL));
    if (!dst_t.is_root(dst_node))
        dst_t.ref(dst_node) |= (src_type & ryml::KEY);

    if (src_t.has_key(src_node) && !dst_t.is_root(dst_node))
        dst_t.set_key(dst_node, dst_t.to_arena(src_t.key(src_node)));
    if (src_t.has_val(src_node))
        dst_t.set_val(dst_node, dst_t.to_arena(src_t.val(src_node)));

    std::vector<size_t> src_children;
    for (size_t c = src_t.first_child(src_node); c != ryml::NONE;
         c = src_t.next_sibling(c))
        if (c != skip) src_children.push_back(c);

    std::vector<size_t> dst_children;
    for (size_t i = 0; i < src_children.size(); i++) {
        ensure_capacity(dst_t);
        dst_children.push_back(dst_t.append_child(dst_node));
    }
    for (size_t i = 0; i < src_children.size(); i++)
        deep_copy_tracked_except(dst_t, dst_children[i], src_t, src_children[i],
                                 skip, prov);
}

static void deep_copy_tracked(ryml::Tree& dst_t, size_t dst_node,
                              const ryml::Tree& src_t, size_t src_node,
                              std::map<size_t, size_t>& prov) {
    deep_copy_tracked_except(dst_t, dst_node, src_t, src_node, ryml::NONE, prov);
}

// Rewrite `prov` (ids -> ids in some intermediate tree) so that it points at
// whatever that intermediate tree was itself derived from, using its own
// provenance. This is what lets `expanded` and `adjunct` be cut out of the
// temporary work tree and still record provenance straight back to `combined`:
// the work tree is discarded, so links through it would dangle. Nodes with no
// entry in `via` (created during expansion, e.g. `destination_pointer`) have
// no source and drop out.
static void chain_prov(std::map<size_t, size_t>& prov,
                       const std::map<size_t, size_t>& via) {
    for (auto it = prov.begin(); it != prov.end();) {
        auto up = via.find(it->second);
        if (up == via.end()) {
            it = prov.erase(it);
        } else {
            it->second = up->second;
            ++it;
        }
    }
}

// Remove every entry for `node` and its descendants from `prov`. Call this
// before ryml removes a subtree, so freed ids that get reused later cannot
// carry a stale provenance link.
static void erase_prov_subtree(const ryml::Tree& t, size_t node,
                               std::map<size_t, size_t>& prov) {
    if (node == ryml::NONE) return;
    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        erase_prov_subtree(t, c, prov);
    prov.erase(node);
}

// After a within-tree duplicate produced `new_node` from `src_node` (an
// isomorphic subtree), walk both in parallel and give each new node the same
// provenance as its template — i.e. new copies point at the same source node
// their template did. Nodes whose template had no provenance stay unmapped.
static void assign_prov_parallel(const ryml::Tree& t, size_t new_node,
                                 size_t src_node,
                                 std::map<size_t, size_t>& prov) {
    auto it = prov.find(src_node);
    if (it != prov.end()) prov[new_node] = it->second;
    size_t nc = t.first_child(new_node);
    size_t sc = t.first_child(src_node);
    while (nc != ryml::NONE && sc != ryml::NONE) {
        assign_prov_parallel(t, nc, sc, prov);
        nc = t.next_sibling(nc);
        sc = t.next_sibling(sc);
    }
}

// Within-tree duplicate of `src` under `parent` (after `after`), recording
// provenance for the whole duplicated subtree.
static size_t duplicate_tracked(ryml::Tree& t, size_t src, size_t parent,
                                size_t after, std::map<size_t, size_t>& prov) {
    size_t nw = t.duplicate(src, parent, after);
    assign_prov_parallel(t, nw, src, prov);
    return nw;
}

// Provenance-tracked version of ryml's duplicate_children_no_rep: copies the
// children of `src` into `dst` (appending after `after`) but skips any child
// whose key already exists in `dst`. Used for `inherit`.
static void duplicate_children_no_rep_tracked(ryml::Tree& t, size_t src,
                                              size_t dst, size_t after,
                                              std::map<size_t, size_t>& prov) {
    for (size_t c = t.first_child(src); c != ryml::NONE;
         c = t.next_sibling(c)) {
        if (t.has_key(c) && t.find_child(dst, t.key(c)) != ryml::NONE)
            continue;  // key already present — do not overwrite
        after = duplicate_tracked(t, c, dst, after, prov);
    }
}

// Build a name->node map for all elements that have a "kind" key.
static void make_ele_map(std::map<std::string, size_t>& emap,
                         const ryml::Tree& t, size_t node) {
    // do nothing if node is a scalar
    if (node == ryml::NONE || t.is_val(node)) return;

    // if node is a map from a name to it's properties and if the properties
    // contain "kind", add the node to the element map
    if (t.is_map(node)) {
        size_t first = t.first_child(node);
        if (first != ryml::NONE && t.has_key(first) && t.is_map(first) &&
            t.has_child(first, ryml::to_csubstr("kind"))) {
            emap.emplace(std::string(t.key(first).str, t.key(first).len),
                         first);
        }
    }

    // if node is a map or sequence, recurse into children
    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        make_ele_map(emap, t, c);
}

// The problems found while building the expanded tree (undefined lattice,
// dangling references, undefined inherit/repeat/fork targets, un-evaluable
// expressions, ...), returned to the caller so it can decide whether to print,
// save, or ignore them. Defined in pals_problem.h, which pals_check.cpp reports
// into as well; pulled into this file's scope so the call sites below read as
// they did when the list was local.
using pals::add_problem;
using pals::Origin;
using pals::ProblemList;
using pals::Severity;

// A short "group.param" (or just "param") location for a value node, for use in
// problem messages. Deliberately shallow so the identical parameter reached
// through several expansion copies yields one message.
static std::string short_location(const ryml::Tree& t, size_t node) {
    std::string key =
        t.has_key(node) ? std::string(t.key(node).str, t.key(node).len) : "";
    size_t p = t.parent(node);
    std::string pk = (p != ryml::NONE && t.has_key(p))
                         ? std::string(t.key(p).str, t.key(p).len)
                         : "";
    if (!pk.empty() && !key.empty()) return pk + "." + key;
    return key;
}

// Forward declaration — handle_fork calls expand, expand calls handle_fork
static void expand(ryml::Tree& t, size_t node,
                   std::map<std::string, size_t>& emap,
                   std::map<size_t, size_t>& prov, ProblemList& problems,
                   size_t branches, std::map<size_t, int>& mp_pass,
                   std::set<size_t>& done);

// Helper: find an element named 'name' within a line sequence. The element can
// be just the scalar name or defined as a map.
static size_t find_in_line(const ryml::Tree& t, size_t line,
                           const std::string& name) {
    for (size_t c = t.first_child(line); c != ryml::NONE;
         c = t.next_sibling(c)) {
        if (t.is_val(c) && std::string(t.val(c).str, t.val(c).len) == name)
            return c;
        if (t.is_map(c)) {
            size_t entry = t.first_child(c);
            if (entry != ryml::NONE && t.has_key(entry) &&
                std::string(t.key(entry).str, t.key(entry).len) == name)
                return entry;
        }
    }
    return ryml::NONE;
}

// Handle a Fork element:
//  1. Reads the ForkP.
//  2. Interprets `new_branch` (fork.md, s:fork.params): "SELF" (the default)
//     and any custom name instantiate a new branch from the BeamLine named by
//     `to_line`; the literal `null` instead points into an existing branch and
//     creates nothing.
//  3. Names the new branch (after `to_line` for SELF, else after `new_branch`).
//  4. Checks the destination is a kind that may be forked to.
//  5. Creates a ForkP.destination_pointer pointing at "destination_element" in
//     the destination branch, and resolves ForkP.new_branch to the name of the
//     branch that was made (or `null` when none was). The pointer is a node id,
//     for use while expansion runs; link_fork_connections later turns it into
//     the `ForkP.forked_to` name that the expanded lattice states the
//     connection by.
static void handle_fork(ryml::Tree& t, size_t fork_node, size_t branches,
                        std::map<std::string, size_t>& emap,
                        std::map<size_t, size_t>& prov, ProblemList& problems,
                        std::map<size_t, int>& mp_pass, std::set<size_t>& done) {
    std::string fork_name = t.has_key(fork_node)
                                ? std::string(t.key(fork_node).str,
                                              t.key(fork_node).len)
                                : "<fork>";
    if (branches == ryml::NONE) {
        add_problem(problems, "Fork element '" + fork_name +
                                  "': no branches to fork into");
        return;
    }

    size_t forkp = t.find_child(fork_node, ryml::to_csubstr("ForkP"));
    if (forkp == ryml::NONE) {
        add_problem(problems,
                    "Fork element '" + fork_name + "': missing ForkP");
        return;
    }
    if (!t.is_map(forkp)) {
        add_problem(problems,
                    "Fork element '" + fork_name +
                        "': ForkP must be a map of parameters (e.g. "
                        "\"to_line: ...\"), not a sequence");
        return;
    }

    // Only `to_line` is required. `destination_element` defaults to the
    // destination branch's beginning (first) element. `new_branch` selects the
    // action (fork.md, s:fork.params):
    //   * "SELF" (the default when unset): `to_line` must name a BeamLine; a new
    //     branch is created named after that beam line (i.e. `to_line`).
    //   * "null": `to_line` must name an existing branch; no new branch is made,
    //     the Fork just points into it.
    //   * any other value: as SELF, but the new branch takes that name.
    std::string to_line = child_val_str(t, forkp, "to_line");
    if (to_line.empty() || to_line == "null") {
        add_problem(problems,
                    "Fork element '" + fork_name +
                        "': ForkP is missing the required field 'to_line'");
        return;
    }
    std::string to_element = child_val_str(t, forkp, "destination_element");
    if (to_element == "null") to_element.clear();

    std::string new_branch = child_val_str(t, forkp, "new_branch");
    bool fork_to_existing = (new_branch == "null");
    std::string branch_name =
        (new_branch.empty() || new_branch == "SELF") ? to_line : new_branch;

    size_t branch_node = ryml::NONE;
    bool created_branch = false;
    if (fork_to_existing) {
        // `null`: point into an existing branch, located by its (original) name.
        for (size_t c = t.first_child(branches); c != ryml::NONE;
             c = t.next_sibling(c)) {
            if (!t.is_map(c)) continue;
            size_t entry = t.first_child(c);
            if (entry != ryml::NONE && t.has_key(entry) &&
                std::string(t.key(entry).str, t.key(entry).len) == to_line) {
                branch_node = entry;
                break;
            }
        }
        if (branch_node == ryml::NONE) {
            add_problem(problems,
                        "Fork element '" + fork_name +
                            "': new_branch is null, but to_line '" + to_line +
                            "' is not an existing branch");
            return;
        }
    } else {
        // SELF or a custom name: instantiate a new branch from the BeamLine.
        if (!emap.count(to_line)) {
            add_problem(problems, "Fork element '" + fork_name + "': to_line '" +
                                      to_line + "' is not a defined BeamLine");
            return;
        }
        size_t def = emap[to_line];
        // Append a new wrapper map to branches, then duplicate the definition
        // into it.
        ensure_capacity(t, t.num_children(def) + 2);
        size_t wrapper = t.append_child(branches);
        t.to_map(wrapper);
        branch_node = duplicate_tracked(t, def, wrapper, ryml::NONE, prov);
        // Rename from the original element name to branch_name.
        t.set_key(branch_node, t.to_arena(ryml::to_csubstr(branch_name)));
        // Expand the new branch so its scalars and inherits are resolved, then
        // mark it done. It has just been appended to `branches`, which an
        // enclosing expand may still be walking; without the mark that walk
        // reaches it and expands it a second time, re-running every Fork inside
        // it and so duplicating both their destination branches and their
        // destination_pointers.
        expand(t, branch_node, emap, prov, problems, branches, mp_pass, done);
        done.insert(branch_node);
        created_branch = true;
    }

    // Find to_element within the new branch's line
    size_t line = t.find_child(branch_node, ryml::to_csubstr("line"));
    if (line == ryml::NONE || !t.is_seq(line)) {
        add_problem(problems, "Fork element '" + fork_name + "': to_line '" +
                                  to_line + "' has no line to fork into");
        return;
    }
    size_t target;
    if (to_element.empty()) {
        // Default destination is the branch's beginning (first) element. A line
        // entry may be a bare scalar or a keyed map; mirror find_in_line and
        // resolve a map entry to its keyed element.
        target = t.first_child(line);
        if (target != ryml::NONE && t.is_map(target))
            target = t.first_child(target);
        if (target == ryml::NONE) {
            add_problem(problems, "Fork element '" + fork_name + "': to_line '" +
                                      to_line +
                                      "' has no beginning element to fork into");
            return;
        }
    } else {
        target = find_in_line(t, line, to_element);
        if (target == ryml::NONE) {
            add_problem(problems, "Fork element '" + fork_name +
                                      "': destination_element '" + to_element +
                                      "' not found in '" + to_line + "'");
            return;
        }
    }

    // A Fork has zero length and a unit transfer map, so to keep the connection
    // unambiguous the destination has to be an element with those same
    // properties: only a Marker, a BeginningEle, or another Fork may be forked
    // to (lattice-construction.md, s:fork). A bare entry that never resolved
    // carries no kind to judge, and is reported as a dangling reference in its
    // own right.
    if (t.is_map(target)) {
        std::string dest_kind = child_val_str(t, target, "kind");
        if (dest_kind != "Marker" && dest_kind != "BeginningEle" &&
            dest_kind != "Fork") {
            std::string dest_name(t.key(target).str, t.key(target).len);
            add_problem(problems,
                        "Fork element '" + fork_name +
                            "': destination element '" + dest_name + "' in '" +
                            to_line + "' has kind '" +
                            (dest_kind.empty() ? "<none>" : dest_kind) +
                            "'; a Fork destination must be a Marker, a "
                            "BeginningEle, or a Fork");
            return;
        }
    }

    // Add ForkP.destination_pointer: <node id of target as string>. It sits in
    // the group beside the `to_line`/`destination_element` it resolves and the
    // `forked_to` it will become, not loose on the element.
    ensure_capacity(t, 2);
    std::string id_str = std::to_string(target);
    size_t fp_child = t.append_child(forkp);
    t.ref(fp_child) |= ryml::KEY | ryml::VAL;
    t.set_key(fp_child, t.to_arena(ryml::to_csubstr("destination_pointer")));
    t.set_val(fp_child, t.to_arena(ryml::to_csubstr(id_str)));

    // Resolve ForkP.new_branch to the branch this Fork actually instantiated:
    // its name, or the literal `null` when the Fork points into a branch that
    // already existed. This materializes the input form (unset == SELF, or a
    // custom name) into the name the expanded lattice really carries, and is
    // what tells downstream code whether a new branch was made -- reference and
    // floor are propagated to the destination only when it is the beginning
    // element of a *new* branch (fork.md), so the bookkeeper seeds propagation
    // for these and never for a `null` fork into an existing branch.
    size_t nb_child = t.find_child(forkp, ryml::to_csubstr("new_branch"));
    if (nb_child == ryml::NONE) {
        ensure_capacity(t, 2);
        nb_child = t.append_child(forkp);
        t.ref(nb_child) |= ryml::KEY | ryml::VAL;
        t.set_key(nb_child, t.to_arena(ryml::to_csubstr("new_branch")));
    }
    t.set_val(nb_child, t.to_arena(ryml::to_csubstr(
                            created_branch ? branch_name : "null")));
}

// True for a YAML scalar meaning boolean true (`multipass: true`, `True`, `1`).
static bool is_true_flag(const std::string& v) {
    return v == "true" || v == "True" || v == "TRUE" || v == "1";
}

// Stamp `multipass_index: idx` on a line entry (a seq wrapper holding one keyed
// element map), unless the element already carries one. An existing index means
// a nearer (more deeply nested) multipass line already claimed the element, and
// per the standard the *first* multipass line up the chain wins, so it is not
// overwritten.
static void set_multipass_index(ryml::Tree& t, size_t entry, int idx) {
    if (entry == ryml::NONE || !t.is_map(entry)) return;
    size_t ele = t.first_child(entry);  // the keyed element map
    if (ele == ryml::NONE || !t.is_map(ele)) return;
    if (t.find_child(ele, ryml::to_csubstr("multipass_index")) != ryml::NONE)
        return;
    ensure_capacity(t);
    size_t mi = t.append_child(ele);
    t.ref(mi) |= ryml::KEY | ryml::VAL;
    t.set_key(mi, t.to_arena(ryml::to_csubstr("multipass_index")));
    t.set_val(mi, t.to_arena(ryml::to_csubstr(std::to_string(idx))));
}

// Stamp the multipass index `pass` on every entry of one instance of a
// multipass line (the run [first, stop), ryml::NONE == to the end of the
// sequence). Entries a nearer multipass line already claimed keep their index.
//
// The multipass index is the *pass number*: the number of times a particle
// will have travelled through the physical element by that point, i.e. the
// ordinal of this instance among the traversals of its multipass line. Every
// element the instance contributes gets the same pass number. Two elements are
// the same physical element when they share a multipass line and sit at the
// same position within it; those matching positions across successive instances
// carry the increasing pass numbers 1, 2, ... .
static void stamp_multipass_pass(ryml::Tree& t, size_t first, size_t stop,
                                 int pass) {
    for (size_t e = first; e != ryml::NONE && e != stop; e = t.next_sibling(e))
        set_multipass_index(t, e, pass);
}

static void expand_seq_range(ryml::Tree& t, size_t node, size_t first,
                             size_t stop, std::map<std::string, size_t>& emap,
                             std::map<size_t, size_t>& prov,
                             ProblemList& problems, size_t branches,
                             std::map<size_t, int>& mp_pass,
                             std::set<size_t>& done);

/**
 * Perform lattice expansion on the element `node`.
 * 1. Substitute scalar elements with their full definition taken from emap.
 * 2. Beamlines that contain "repeat: n" have their contents repeated |n| times,
 *    each copy expanded in turn like any other entry of the enclosing line. A
 *    negative n reflects: the repeated elements come out in reverse order.
 * 3. Elements that contain "inherit: ancestor" have the contents of ancestor
 * copied into element.
 * 4. A beamline referenced by bare name inside a `line:` is a sub-line: its
 *    `line:` contents are spliced directly into the enclosing line. When the
 *    sub-line is `multipass`, the spliced run is stamped with its pass number.
 *
 * `mp_pass` counts, per multipass line definition, how many instances of that
 * line have been spliced so far; expansion visits them in traversal order, so
 * the count is the pass number to stamp on the next instance.
 */
static void expand(ryml::Tree& t, size_t node,
                   std::map<std::string, size_t>& emap,
                   std::map<size_t, size_t>& prov, ProblemList& problems,
                   size_t branches, std::map<size_t, int>& mp_pass,
                   std::set<size_t>& done) {
    if (node == ryml::NONE) return;
    // A branch a Fork built was expanded eagerly, at the point the Fork had to
    // look inside it for its destination element. Expansion is not idempotent
    // (a second pass re-runs the Forks it contains), so it is only ever done
    // once. See handle_fork.
    if (done.count(node)) return;

    // Sequence — handle 'repeat'
    if (t.is_seq(node)) {
        expand_seq_range(t, node, t.first_child(node), ryml::NONE, emap, prov,
                         problems, branches, mp_pass, done);
        return;
    }

    // Standalone scalar value
    if (t.is_val(node) && !t.has_key(node)) {
        std::string name(t.val(node).str, t.val(node).len);
        size_t p = t.parent(node);
        bool in_line = (p != ryml::NONE && t.has_key(p) &&
                        t.key(p) == ryml::to_csubstr("line"));
        auto it = emap.find(name);
        if (it != emap.end()) {
            size_t def = it->second;
            size_t def_line = t.find_child(def, ryml::to_csubstr("line"));
            bool is_beamline = child_val_str(t, def, "kind") == "BeamLine";

            // Sub-line: a beamline referenced by bare name inside a `line:`
            // contributes its `line:` contents directly to the enclosing line
            // rather than surviving as a nested BeamLine. (A bare name in a
            // `branches:` sequence is a branch, not a sub-line — it falls
            // through to plain substitution and is stripped of its kind later.)
            if (in_line && is_beamline && def_line != ryml::NONE &&
                t.is_seq(def_line)) {
                bool multipass =
                    is_true_flag(child_val_str(t, def, "multipass"));
                // Splice the sub-line's entries in front of this reference, then
                // drop the reference itself. `before`/`stop` bracket the spliced
                // run; both are outside it and stay put while it is expanded.
                size_t before = t.prev_sibling(node);
                size_t stop = t.next_sibling(node);
                size_t after = before;
                for (size_t c2 = t.first_child(def_line); c2 != ryml::NONE;
                     c2 = t.next_sibling(c2)) {
                    ensure_capacity(t, 2);
                    after = duplicate_tracked(t, c2, p, after, prov);
                }
                erase_prov_subtree(t, node, prov);
                t.remove(node);

                // Expand the spliced run in place: element references become
                // element maps, and any nested sub-lines flatten (assigning
                // their own, nearer, multipass indices) in turn.
                size_t first = (before == ryml::NONE) ? t.first_child(p)
                                                      : t.next_sibling(before);
                for (size_t cur = first; cur != ryml::NONE && cur != stop;) {
                    size_t nx = t.next_sibling(cur);
                    expand(t, cur, emap, prov, problems, branches, mp_pass, done);
                    cur = nx;
                }

                // The spliced run is one instance (one pass) of this multipass
                // sub-line: stamp every entry a nearer multipass sub-line did
                // not already claim with this instance's pass number. Instances
                // of a given line are visited in traversal order, so the running
                // per-definition count is that pass number.
                if (multipass)
                    stamp_multipass_pass(t, first, stop, ++mp_pass[def]);
                return;
            }

            // Element (or a beamline outside a `line:`): substitute in place.
            ensure_capacity(t, 2);
            t.change_type(node, ryml::MAP);
            duplicate_tracked(t, def, node, ryml::NONE, prov);
            expand(t, node, emap, prov, problems, branches, mp_pass, done);
        } else {
            // A bare entry in a `line:` or `branches:` sequence names an element
            // or beamline; if it is not defined, the reference is dangling.
            std::string pk = (p != ryml::NONE && t.has_key(p))
                                 ? std::string(t.key(p).str, t.key(p).len)
                                 : "";
            if (pk == "line" || pk == "branches")
                add_problem(problems, "reference to undefined element or line '" +
                                          name + "'");
        }
        return;
    }

    // Map — handle inherit, detect Lattice/Fork, recurse
    if (t.is_map(node)) {
        // Inherit: merge fields from named element (existing keys win)
        size_t inherit_id = t.find_child(node, ryml::to_csubstr("inherit"));
        if (inherit_id != ryml::NONE) {
            std::string name(t.val(inherit_id).str, t.val(inherit_id).len);
            if (emap.count(name)) {
                ensure_capacity(t, t.num_children(emap[name]) + 1);
                duplicate_children_no_rep_tracked(t, emap[name], node,
                                                  ryml::NONE, prov);
            } else {
                add_problem(problems,
                            "inherit: '" + name + "' is not defined");
            }
        }

        // Detect kind to set context or trigger fork handling
        std::string kind = child_val_str(t, node, "kind");
        size_t node_branches = branches;
        if (kind == "Lattice") {
            node_branches = t.find_child(node, ryml::to_csubstr("branches"));
        } else if (kind == "Fork") {
            handle_fork(t, node, branches, emap, prov, problems, mp_pass, done);
        }

        size_t original_size = t.num_children(node);
        size_t c = t.first_child(node);
        for (size_t i = 0; i < original_size && c != ryml::NONE;
             i++, c = t.next_sibling(c))
            expand(t, c, emap, prov, problems, node_branches, mp_pass, done);
    }
}

/**
 * Expand the entries [`first`, `stop`) of the sequence `node` -- `stop` ==
 * ryml::NONE meaning "to the end" -- handling the `repeat` of each entry.
 *
 * Split out of expand() because `repeat` belongs to a line *entry* but is read
 * from the sequence holding it: expanding an entry on its own walks straight
 * past it. The copies a `repeat` splices in are entries of that same sequence
 * and may carry `repeat`s of their own, so they go back through this loop
 * rather than being expanded one at a time.
 */
static void expand_seq_range(ryml::Tree& t, size_t node, size_t first,
                             size_t stop, std::map<std::string, size_t>& emap,
                             std::map<size_t, size_t>& prov,
                             ProblemList& problems, size_t branches,
                             std::map<size_t, int>& mp_pass,
                             std::set<size_t>& done) {
    size_t child = first;
    // loop over all children
    while (child != ryml::NONE && child != stop) {
        size_t next = t.next_sibling(child);
        if (t.is_map(child) && t.has_children(child)) {
            // first_child because elements are defined as maps with only
            // one key
            size_t entry = t.first_child(child);
            if (t.has_key(entry) && t.is_map(entry)) {
                size_t repeat_id =
                    t.find_child(entry, ryml::to_csubstr("repeat"));
                if (repeat_id != ryml::NONE && t.has_val(repeat_id)) {
                    std::string cnt_txt(t.val(repeat_id).str,
                                        t.val(repeat_id).len);
                    std::string target(t.key(entry).str, t.key(entry).len);

                    // stoll stops at the first character it cannot use and
                    // reports how far it got, so require the whole value to be
                    // consumed: "3x" is a typo, not a count of 3.
                    //
                    // A negative count is a reflection: |count| copies whose
                    // elements come out in reverse order (beamlines.md,
                    // s:repetition). Reversed order is not direction reversal
                    // -- that is the separate `direction` component -- so the
                    // copies themselves are left untouched and only their order
                    // is turned around, below.
                    bool count_ok = true;
                    long long count = 0;
                    try {
                        size_t used = 0;
                        count = std::stoll(cnt_txt, &used);
                        if (used != cnt_txt.size()) count_ok = false;
                    } catch (...) {
                        count_ok = false;
                    }
                    const bool reflect = count < 0;
                    const long long copies = reflect ? -count : count;

                    if (!count_ok) {
                        add_problem(problems, "repeat: invalid count '" +
                                                  cnt_txt + "' for '" +
                                                  target + "'");
                    } else if (!emap.count(target)) {
                        // check if the beamline to be repeated has been
                        // defined in the file
                        add_problem(problems, "repeat: beamline '" + target +
                                                  "' is not defined");
                    } else {
                        size_t def = emap[target];
                        size_t line_id =
                            t.find_child(def, ryml::to_csubstr("line"));
                        // `before`/`next` bracket the run about to be
                        // spliced in: both are outside it and stay put
                        // while it is built, so they still bracket it once
                        // `child` itself is gone.
                        size_t before = t.prev_sibling(child);
                        size_t after = before;
                        // if beamline to be repeated has a line, duplicate
                        // it `copies` times, else just duplicate the name
                        // of the beamline `copies` times
                        if (line_id != ryml::NONE && t.is_seq(line_id)) {
                            for (long long r = 0; r < copies; r++)
                                for (size_t c2 = t.first_child(line_id);
                                     c2 != ryml::NONE;
                                     c2 = t.next_sibling(c2)) {
                                    ensure_capacity(t, 2);
                                    after = duplicate_tracked(t, c2, node,
                                                              after, prov);
                                }
                        } else {
                            for (long long r = 0; r < copies; r++) {
                                ensure_capacity(t, 2);
                                size_t wrapper =
                                    t.insert_child(node, after);
                                t.ref(wrapper) |= ryml::MAP;
                                duplicate_tracked(t, def, wrapper,
                                                  ryml::NONE, prov);
                                after = wrapper;
                            }
                        }
                        erase_prov_subtree(t, child, prov);
                        t.remove(child);

                        // Expand the copies just spliced in. They are
                        // duplicates of the repeated definition's own
                        // entries -- element references, nested sub-lines,
                        // and `repeat`s of their own -- so they get exactly
                        // the pass this loop would have given them had they
                        // been written out by hand. Skipping them left a
                        // branch of bare names that extracted as an empty
                        // lattice, and reported nothing.
                        expand_seq_range(t, node,
                                         (before == ryml::NONE)
                                             ? t.first_child(node)
                                             : t.next_sibling(before),
                                         next, emap, prov, problems, branches,
                                         mp_pass, done);

                        // A negative count reflects the run just spliced in.
                        // Reversing it only now, after the copies are expanded,
                        // is what makes this a reversal of *elements*: by this
                        // point every sub-line the copies held has been spliced
                        // inline, so the run is the flat element list the
                        // standard defines the reflection on, and a reflection
                        // nested inside this one has already turned its own
                        // stretch around.
                        if (reflect) {
                            std::vector<size_t> run;
                            for (size_t cur =
                                     (before == ryml::NONE)
                                         ? t.first_child(node)
                                         : t.next_sibling(before);
                                 cur != ryml::NONE && cur != next;
                                 cur = t.next_sibling(cur))
                                run.push_back(cur);
                            // Moving each entry in turn to the head of the
                            // run leaves them in the opposite order.
                            for (size_t i = 1; i < run.size(); i++)
                                t.move(run[i], before);
                        }

                        child = next;
                        continue;
                    }
                    // Either problem path falls through to leave the entry
                    // exactly as written. Only a repeat we fully understood
                    // removes it: dropping an entry on the strength of a
                    // count we could not read would delete part of the
                    // lattice, leaving a plausible-looking `line: []` that
                    // is only explained by a problem the caller may not
                    // check.
                }
            }
        }
        expand(t, child, emap, prov, problems, branches, mp_pass, done);
        child = next;
    }
}

/**
 * Give every root branch written as `- name: {...}` the `inherit` it implies.
 *
 * A branch's `inherit` names its root BeamLine and is optional: "Default is the
 * name of the Branch" (lattice-construction.md, s:lattice.construct). So
 *
 *     branches:
 *       - ln:
 *           periodic: false
 *
 * is the branch `ln` built from the BeamLine `ln`, overriding its `periodic`.
 * Expansion reaches a definition only through a bare name or an explicit
 * `inherit`, so the default is supplied here, before expand() runs, by writing
 * the key back out as an `inherit`. Appending it leaves the entry's own keys
 * ahead of the inherited ones, which is what makes `periodic` an override.
 *
 * Only root branches are written this way; a Fork builds its branch by copying
 * the `to_line` definition outright and never arrives here.
 */
static void default_branch_inherit(ryml::Tree& t, size_t lat_node,
                                   const std::map<std::string, size_t>& emap) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;

    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        // A bare `- this_line` is a scalar and needs nothing: name substitution
        // already brings the definition in.
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch) || !t.has_key(branch))
            continue;
        // An explicit `inherit` is the root line, and a branch carrying its own
        // `line` is already the finished article; neither wants a default.
        if (t.find_child(branch, ryml::to_csubstr("inherit")) != ryml::NONE)
            continue;
        if (t.find_child(branch, ryml::to_csubstr("line")) != ryml::NONE)
            continue;
        std::string name(t.key(branch).str, t.key(branch).len);
        // A name that is not defined is left alone: the empty branch it produces
        // is reported by check_branches_expanded, which says so in those terms
        // rather than as a failed inherit.
        if (emap.find(name) == emap.end()) continue;

        ensure_capacity(t, 2);
        size_t inh = t.append_child(branch);
        t.ref(inh) |= ryml::KEY | ryml::VAL;
        t.set_key(inh, t.to_arena(ryml::to_csubstr("inherit")));
        t.set_val(inh, t.to_arena(ryml::to_csubstr(name)));
    }
}

/**
 * Report every branch that came out of expansion holding no elements.
 *
 * A branch is the ordered element list its root BeamLine expands to, so an
 * empty one is always a mistake in the file -- most often a root line that was
 * never defined. Without this the emptiness is silent, and surfaces only
 * indirectly, as whatever downstream check first goes looking for an element
 * that should have been there.
 */
static void check_branches_expanded(const ryml::Tree& t, size_t lat_node,
                                    const std::map<std::string, size_t>& emap,
                                    ProblemList& problems) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;

    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch) || !t.has_key(branch))
            continue;
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line != ryml::NONE && t.is_seq(line) && t.num_children(line) > 0)
            continue;

        std::string name(t.key(branch).str, t.key(branch).len);
        // With no `inherit` the root line is the branch's own name. An explicit
        // `inherit` that names nothing is already reported by expand(), so only
        // the defaulted name is diagnosed here.
        bool defaulted =
            t.find_child(branch, ryml::to_csubstr("inherit")) == ryml::NONE;
        if (defaulted && emap.find(name) == emap.end())
            add_problem(problems, "branch '" + name +
                                      "': no root BeamLine '" + name +
                                      "' is defined");
        else
            add_problem(problems,
                        "branch '" + name + "': expanded to no elements");
    }
}

/**
 * Drop `kind: BeamLine` from every branch of the expanded lattice `lat_node`.
 *
 * A `branches:` entry is a branch, not a BeamLine. It is instantiated from a
 * root BeamLine, and a branch carries no `kind` of its own -- its only optional
 * components are `inherit` and `periodic`. Every route a branch reaches its
 * contents by copies that root BeamLine's definition in wholesale, and so drags
 * the definition's `kind: BeamLine` along: name substitution of a bare
 * `- this_line`, an explicit `inherit: that_ring`, and a Fork building a new
 * branch out of its `to_line`. Rather than special-case each route, strip the
 * key once here, after expansion has produced every branch.
 *
 * Only branches carry a `kind: BeamLine` to strip. Sub-lines -- beamlines
 * referenced inside a `line:` -- do not survive expansion: their contents are
 * spliced into the enclosing line, so no nested BeamLine remains to consider.
 */
static void strip_branch_kinds(ryml::Tree& t, size_t lat_node,
                               std::map<size_t, size_t>& prov) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;

    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        // Both the written form (`- name: {...}`) and a substituted bare name
        // land as a map wrapping the single keyed branch node.
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        // Anything other than BeamLine is a branch built from a definition that
        // is not a beamline at all. That is a mistake in the file, and leaving
        // the kind in place keeps the evidence of it visible.
        if (child_val_str(t, branch, "kind") != "BeamLine") continue;
        size_t kind = t.find_child(branch, ryml::to_csubstr("kind"));
        erase_prov_subtree(t, kind, prov);
        t.remove(kind);
    }
}

/**
 * Remove the `Placeholder` elements from every branch of the expanded lattice
 * `lat_node`. A Placeholder in the file holds a place during construction and
 * never survives into the expanded lattice (lattice-element-kinds.md,
 * s:placeholder); the one the bookkeeper caps each branch with is added later.
 */
static void remove_placeholders(ryml::Tree& t, size_t lat_node,
                                std::map<size_t, size_t>& prov) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;
        for (size_t le = t.first_child(line); le != ryml::NONE;) {
            size_t next = t.next_sibling(le);
            size_t def = t.is_map(le) ? t.first_child(le) : ryml::NONE;
            if (def != ryml::NONE && t.is_map(def) &&
                child_val_str(t, def, "kind") == "Placeholder") {
                erase_prov_subtree(t, le, prov);
                t.remove(le);
            }
            le = next;
        }
    }
}

/**
 * Number the elements of every branch whose root line is itself `multipass`.
 *
 * A branch's own line is the top of the sub-line chain, so a `multipass: true`
 * branch is the nearest multipass line for any of its elements not already
 * claimed by a nested multipass sub-line (those were numbered as they flattened,
 * and keep their nearer index). Runs after expansion, once every branch line
 * holds its full flat element sequence.
 *
 * Each branch instance is one traversal (one pass) of its root line, so all of
 * its still-unclaimed elements share a pass number. Physical element sets group
 * by the root line *definition*, which can span branches: two branches built
 * from the same root line are two passes through the same physical elements, so
 * their matching positions carry successive pass numbers. Provenance recovers
 * that shared definition — branches copied from one root map back to the same
 * combined node — so the per-definition count is the pass number to stamp.
 */
static void number_multipass_branches(ryml::Tree& t, size_t lat_node,
                                      std::map<size_t, size_t>& prov) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;

    std::map<size_t, int> mp_pass;  // root-line definition -> traversals so far
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        if (!is_true_flag(child_val_str(t, branch, "multipass"))) continue;
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;
        // Key on the branch's definition. A branch with no recorded provenance
        // (none should occur here) falls back to its own node id, which is
        // unique and so numbers it as a lone first pass.
        auto it = prov.find(branch);
        size_t def_key = (it != prov.end()) ? it->second : branch;
        stamp_multipass_pass(t, t.first_child(line), ryml::NONE,
                             ++mp_pass[def_key]);
    }
}

// ============================================================
// FILE REFERENCE PATHS (include / load)
// ============================================================
//
// A file named by an `include` or a `load` is located relative to the file that
// names it, not to the process working directory, so a set of files that refer
// to each other can be moved or checked out anywhere as a unit. Resolution is
// purely lexical -- the naming file's directory is prepended and the result is
// folded -- so one file reaches the `original` master tree under one key
// whatever route led to it, and a reference cycle is recognisable by that key.

// Everything up to the last '/' in `p`; empty when `p` names a bare file.
static std::string path_dir(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash);
}

// Collapse "." and "x/.." segments and repeated separators. Leading ".." is
// kept (there is no way to know what it resolves to without touching the
// filesystem) except on an absolute path, where it cannot rise above "/".
static std::string fold_path(const std::string& p) {
    const bool absolute = !p.empty() && p[0] == '/';
    std::vector<std::string> parts;
    for (size_t i = 0; i < p.size();) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg != "..") {
            parts.push_back(seg);
        } else if (!parts.empty() && parts.back() != "..") {
            parts.pop_back();
        } else if (!absolute) {
            parts.push_back(seg);
        }
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += "/";
        out += parts[k];
    }
    if (out.empty()) out = ".";
    return out;
}

// The path of the file `ref` names, as named from inside the file at `from`.
static std::string resolve_path(const std::string& from,
                                const std::string& ref) {
    if (!ref.empty() && ref[0] == '/') return fold_path(ref);
    std::string dir = path_dir(from);
    return fold_path(dir.empty() ? ref : dir + "/" + ref);
}

/**
 * Recursive helper for make_combined_from_original. Starting from `node` in the
 * combined tree `t`, replace every "include: filename" element with the
 * contents of that file, sourced from the already-parsed `original` tree so
 * provenance can be recorded. Also recurses into spliced content to handle
 * nested include statements.
 *
 * `path` is the resolved path of the file whose contents `node` sits in, which
 * is what its include references are relative to. Spliced-in content is walked
 * with the included file's own path, so a chain of includes each resolves
 * against the file that wrote it.
 */
static void make_combined_splice(ryml::Tree& t, size_t node, ParsedData* orig,
                                 const std::string& path,
                                 std::map<size_t, size_t>& prov) {
    if (node == ryml::NONE) return;

    if (t.is_seq(node)) {
        size_t child = t.first_child(node);
        while (child != ryml::NONE) {
            size_t next = t.next_sibling(child);

            // Seq elements are anonymous MAP wrappers, so - include: "file"
            // looks like:
            //   anon wrapper (MAP, no key) -> KEYVAL(key="include", val="file")
            std::string filename;
            bool is_include = false;
            if (!t.has_key(child) && t.is_map(child) &&
                t.num_children(child) == 1) {
                size_t inner = t.first_child(child);
                if (inner != ryml::NONE && t.has_key(inner) &&
                    t.key(inner) == ryml::to_csubstr("include") &&
                    t.has_val(inner)) {
                    is_include = true;
                    filename = std::string(t.val(inner).str, t.val(inner).len);
                }
            }

            if (is_include) {
                // Look up the included file's raw contents in `original`. It is
                // stored keyed by its path resolved against the naming file --
                // the same key make_original registered it under.
                const std::string inc_path = resolve_path(path, filename);
                ryml::Tree& ot = orig->tree;
                size_t inc_root =
                    ot.find_child(ot.root_id(), ryml::to_csubstr(inc_path));
                size_t after = child;
                std::vector<size_t> inserted;
                if (inc_root != ryml::NONE) {
                    // Splice each child of the included file's root into this
                    // sequence, recording provenance into `original`.
                    if (ot.is_seq(inc_root)) {
                        for (size_t c = ot.first_child(inc_root);
                             c != ryml::NONE; c = ot.next_sibling(c)) {
                            ensure_capacity(t);
                            size_t n = t.insert_child(node, after);
                            deep_copy_tracked(t, n, ot, c, prov);
                            inserted.push_back(n);
                            after = n;
                        }
                    } else {
                        // Included root is not a sequence — insert as a single
                        // element
                        ensure_capacity(t);
                        size_t n = t.insert_child(node, after);
                        deep_copy_tracked(t, n, ot, inc_root, prov);
                        inserted.push_back(n);
                    }
                }
                // Recurse into inserted nodes to handle nested includes. Those
                // are the included file's own, so they resolve against it.
                for (size_t n : inserted)
                    make_combined_splice(t, n, orig, inc_path, prov);
                erase_prov_subtree(t, child, prov);
                t.remove(child);
                child = next;
                continue;
            }

            make_combined_splice(t, child, orig, path, prov);
            child = next;
        }
        return;
    }

    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        make_combined_splice(t, c, orig, path, prov);
}

// ============================================================
// LOAD
// ============================================================
//
// `load` and `include` both draw several files into the combined tree, but
// where an include splices a file's contents in verbatim at the point it is
// written, a load merges whole files subnode by subnode under the `PALS` root.
// The typical use is a layout file plus one of several settings files, joined
// by a small file that names both.
//
// Includes are resolved first, so each file is complete before files are
// combined; loading is bottom-up, so a loaded file that loads further files is
// itself a single merged file by the time it is merged into its joiner.

// The `load` list entry standing for the joiner file's own contents. Absent
// from the list, they are merged last.
static const char* const LOAD_SELF = "SELF";

// Structural equality of two subtrees: same shape, keys and values throughout.
// `load` allows two files to supply the same dictionary entry only when they
// agree on it, and this is what "agree" means.
static bool nodes_equal(const ryml::Tree& t, size_t a, size_t b) {
    if (a == ryml::NONE || b == ryml::NONE) return a == b;
    if (t.is_map(a) != t.is_map(b) || t.is_seq(a) != t.is_seq(b)) return false;
    if (t.has_key(a) != t.has_key(b)) return false;
    if (t.has_key(a) && t.key(a) != t.key(b)) return false;
    if (t.has_val(a) != t.has_val(b)) return false;
    if (t.has_val(a) && t.val(a) != t.val(b)) return false;
    size_t ca = t.first_child(a), cb = t.first_child(b);
    for (; ca != ryml::NONE && cb != ryml::NONE;
         ca = t.next_sibling(ca), cb = t.next_sibling(cb))
        if (!nodes_equal(t, ca, cb)) return false;
    return ca == ryml::NONE && cb == ryml::NONE;
}

// Combine two `version` strings. Files written against different versions of
// the schema keep both on record rather than one silently winning, so the
// combined value is the distinct versions in first-seen order, comma delimited.
// `have` may already be such a list from an earlier merge.
static std::string merge_versions(const std::string& have,
                                  const std::string& add) {
    std::vector<std::string> seen;
    auto note = [&seen](const std::string& raw) {
        size_t b = raw.find_first_not_of(" \t");
        if (b == std::string::npos) return;
        size_t e = raw.find_last_not_of(" \t");
        std::string v = raw.substr(b, e - b + 1);
        if (std::find(seen.begin(), seen.end(), v) == seen.end())
            seen.push_back(v);
    };
    for (size_t i = 0; i <= have.size();) {
        size_t j = have.find(',', i);
        if (j == std::string::npos) j = have.size();
        note(have.substr(i, j - i));
        i = j + 1;
    }
    note(add);
    std::string out;
    for (size_t k = 0; k < seen.size(); ++k) {
        if (k) out += ", ";
        out += seen[k];
    }
    return out;
}

/**
 * Merge one file's `PALS` subnodes into the accumulating `dest` map.
 *
 * A subnode `dest` does not have yet is copied in whole. Otherwise the two are
 * combined by type: list subnodes (`notes`, `authors`, `facility`, ...)
 * concatenate, so the combined list keeps the order of the `load` list and,
 * within each file, the order written there; Dict subnodes
 * (`extension_labels`) take the union, with a duplicate entry discarded when
 * the two files agree on it and reported when they do not; and `version`
 * collects the distinct version strings. Any other disagreement over a plain
 * value is reported and the value already in `dest` -- the earlier file's --
 * stands.
 *
 * `src_name` names the file `src` came from, for problem messages.
 */
static void merge_pals_into(ryml::Tree& t, size_t dest, size_t src,
                            const std::string& src_name,
                            std::map<size_t, size_t>& prov,
                            ProblemList& problems) {
    if (dest == ryml::NONE || src == ryml::NONE || !t.is_map(src)) return;

    for (size_t sc = t.first_child(src); sc != ryml::NONE;
         sc = t.next_sibling(sc)) {
        if (!t.has_key(sc)) continue;
        // Held as a std::string: writing into the tree's arena below can
        // relocate it, and with it any csubstr pointing into it.
        const std::string key(t.key(sc).str, t.key(sc).len);
        const size_t dc = t.find_child(dest, ryml::to_csubstr(key));

        if (dc == ryml::NONE) {
            ensure_capacity(t, 2);
            duplicate_tracked(t, sc, dest, t.last_child(dest), prov);
            continue;
        }

        if (key == "version" && t.has_val(dc) && t.has_val(sc)) {
            const std::string merged =
                merge_versions(std::string(t.val(dc).str, t.val(dc).len),
                               std::string(t.val(sc).str, t.val(sc).len));
            t.set_val(dc, t.to_arena(ryml::to_csubstr(merged)));
            continue;
        }

        if (t.is_seq(dc) && t.is_seq(sc)) {
            size_t after = t.last_child(dc);
            for (size_t e = t.first_child(sc); e != ryml::NONE;
                 e = t.next_sibling(e)) {
                ensure_capacity(t, 2);
                after = duplicate_tracked(t, e, dc, after, prov);
            }
            continue;
        }

        if (t.is_map(dc) && t.is_map(sc)) {
            size_t after = t.last_child(dc);
            for (size_t e = t.first_child(sc); e != ryml::NONE;
                 e = t.next_sibling(e)) {
                if (!t.has_key(e)) continue;
                const std::string sub(t.key(e).str, t.key(e).len);
                size_t have = t.find_child(dc, ryml::to_csubstr(sub));
                if (have == ryml::NONE) {
                    ensure_capacity(t, 2);
                    after = duplicate_tracked(t, e, dc, after, prov);
                } else if (!nodes_equal(t, have, e)) {
                    add_problem(problems, "load: '" + src_name +
                                              "' disagrees on the value of '" +
                                              key + "." + sub + "'");
                }
            }
            continue;
        }

        if (t.has_val(dc) && t.has_val(sc) && t.val(dc) == t.val(sc)) continue;

        add_problem(problems, "load: '" + src_name +
                                  "' disagrees on the value of '" + key + "'");
    }
}

/**
 * Resolve one file's `load` list, in place, replacing its `PALS` node with the
 * merge of every file it names.
 *
 * `pals` is the file's `PALS` node in the combined tree and `path` the resolved
 * path of the file it came from, which its own load references are relative to.
 * Each loaded file is staged under a scratch node: copied out of `original`,
 * spliced for its own includes, then resolved for its own `load` -- so it is a
 * single complete file before it takes part in this merge, and nesting works to
 * any depth. The joiner's own contents are staged the same way, which is what
 * lets `SELF` sit anywhere in the order: `pals` can then be emptied and refilled
 * with the merge of every source, taken in load-list order.
 *
 * `active` holds the files whose loads are being resolved further up the
 * recursion, so a cycle is reported rather than followed forever.
 */
static void resolve_loads(ryml::Tree& t, size_t pals, ParsedData* orig,
                          const std::string& path,
                          std::map<size_t, size_t>& prov,
                          std::set<std::string>& active,
                          ProblemList& problems) {
    if (pals == ryml::NONE || !t.is_map(pals)) return;
    size_t load = t.find_child(pals, ryml::to_csubstr("load"));
    if (load == ryml::NONE) return;

    std::vector<std::string> refs;
    bool has_self = false;
    if (t.is_seq(load)) {
        for (size_t e = t.first_child(load); e != ryml::NONE;
             e = t.next_sibling(e)) {
            if (!t.has_val(e)) continue;
            refs.push_back(std::string(t.val(e).str, t.val(e).len));
            if (refs.back() == LOAD_SELF) has_self = true;
        }
    } else if (t.has_val(load)) {
        // A lone filename, written without the list punctuation.
        refs.push_back(std::string(t.val(load).str, t.val(load).len));
    }
    if (!has_self) refs.push_back(LOAD_SELF);

    // The `load` node is a directive, not content: it takes no part in the
    // merge and must not survive into the combined tree.
    erase_prov_subtree(t, load, prov);
    t.remove(load);

    // Staging area. Keyed by the path each file came from, which makes a dumped
    // intermediate tree readable; nothing looks these keys up.
    ensure_capacity(t, 2);
    size_t scratch = t.append_child(t.root_id());
    t.to_map(scratch, t.to_arena(ryml::to_csubstr("__load")));

    // The joiner's own contents, put aside before `pals` is emptied.
    ensure_capacity(t, 2);
    size_t self_node = t.append_child(scratch);
    t.to_map(self_node, t.to_arena(ryml::to_csubstr(LOAD_SELF)));
    {
        size_t after = ryml::NONE;
        for (size_t c = t.first_child(pals); c != ryml::NONE;
             c = t.next_sibling(c)) {
            ensure_capacity(t, 2);
            after = duplicate_tracked(t, c, self_node, after, prov);
        }
    }

    std::vector<size_t> sources;     // PALS nodes to merge, in load order
    std::vector<std::string> names;  // where each came from, for messages

    ryml::Tree& ot = orig->tree;
    for (const std::string& ref : refs) {
        if (ref == LOAD_SELF) {
            sources.push_back(self_node);
            names.push_back(path);
            continue;
        }

        const std::string sub = resolve_path(path, ref);
        if (active.count(sub)) {
            add_problem(problems, "load: '" + path + "' loads '" + sub +
                                      "', which is already being loaded");
            continue;
        }
        size_t src_root = ot.find_child(ot.root_id(), ryml::to_csubstr(sub));
        if (src_root == ryml::NONE) {
            add_problem(problems, "load: could not read '" + sub +
                                      "', loaded from '" + path + "'");
            continue;
        }

        ensure_capacity(t, 2);
        size_t stage = t.append_child(scratch);
        deep_copy_tracked(t, stage, ot, src_root, prov);
        make_combined_splice(t, stage, orig, sub, prov);

        size_t sub_pals = t.find_child(stage, ryml::to_csubstr("PALS"));
        if (sub_pals == ryml::NONE) {
            add_problem(problems,
                        "load: '" + sub + "' has no PALS node to load");
            continue;
        }
        active.insert(sub);
        resolve_loads(t, sub_pals, orig, sub, prov, active, problems);
        active.erase(sub);

        sources.push_back(sub_pals);
        names.push_back(sub);
    }

    for (size_t c = t.first_child(pals); c != ryml::NONE;) {
        size_t next = t.next_sibling(c);
        erase_prov_subtree(t, c, prov);
        t.remove(c);
        c = next;
    }

    for (size_t i = 0; i < sources.size(); ++i)
        merge_pals_into(t, pals, sources[i], names[i], prov, problems);

    erase_prov_subtree(t, scratch, prov);
    t.remove(scratch);
}

/**
 * Makes the combined lattice tree by deep-copying the top-level file's contents
 * out of the `original` tree, splicing in every include (including nested ones)
 * from `original`, and then merging in every `load`ed file. Records provenance
 * mapping each combined node to the original node it was copied from.
 *
 * @param orig     The already-built `original` tree (see make_original).
 * @param filename The top-level file's resolved path, used to find its entry in
 *                 `original` and to resolve the references it makes.
 */
static YAMLTreeHandle make_combined_from_original(ParsedData* orig,
                                                  const std::string& filename,
                                                  ProblemList& problems) {
    if (!orig) return nullptr;
    ParsedData* data = new ParsedData();
    ryml::Tree& t = data->tree;
    t.reserve(t.capacity() + 128);
    t.reserve_arena(t.arena_capacity() + 65536);

    ryml::Tree& ot = orig->tree;
    // The top-level file is stored in `original` keyed by its path.
    size_t top = ot.find_child(ot.root_id(), ryml::to_csubstr(filename));
    if (top == ryml::NONE) top = ot.first_child(ot.root_id());
    if (top == ryml::NONE) return data;

    deep_copy_tracked(t, t.root_id(), ot, top, data->provenance);
    make_combined_splice(t, t.root_id(), orig, filename, data->provenance);

    // Loads combine whole files, so they run once each file is itself complete.
    std::set<std::string> active{filename};
    resolve_loads(t, t.find_child(t.root_id(), ryml::to_csubstr("PALS")), orig,
                  filename, data->provenance, active, problems);
    return data;
}

// helper function for find_lattice
static size_t find_lattice_recursive(const ryml::Tree& t, size_t node,
                                     const std::string& name,
                                     size_t& last_lattice,
                                     std::string& use_name) {
    if (!t.is_map(node) && !t.is_seq(node)) return ryml::NONE;

    for (size_t c = t.first_child(node); c != ryml::NONE;
         c = t.next_sibling(c)) {
        // Track "use" statements — last one wins
        if (t.has_key(c) && t.key(c) == ryml::to_csubstr("use") &&
            t.has_val(c)) {
            use_name = std::string(t.val(c).str, t.val(c).len);
            continue;
        }

        // Check if this node is a lattice (map with kind: Lattice)
        bool is_lattice = false;
        if (t.is_map(c)) {
            for (size_t child = t.first_child(c); child != ryml::NONE;
                 child = t.next_sibling(child)) {
                if (t.has_key(child) &&
                    t.key(child) == ryml::to_csubstr("kind") &&
                    t.has_val(child) &&
                    t.val(child) == ryml::to_csubstr("Lattice")) {
                    is_lattice = true;
                    break;
                }
            }
        }

        if (is_lattice) {
            if (!name.empty()) {
                if (t.has_key(c) &&
                    t.key(c) == ryml::csubstr(name.data(), name.size()))
                    return c;
            } else {
                last_lattice = c;
            }
            continue;
        }

        size_t found =
            find_lattice_recursive(t, c, name, last_lattice, use_name);
        if (found != ryml::NONE) return found;
    }

    return ryml::NONE;
}

/**
 * Finds the lattice to be expanded as specified in make_expanded_from_combined.
 */
static size_t find_lattice(ryml::Tree& t, const std::string& name) {
    size_t last_lattice = ryml::NONE;
    std::string use_name;

    size_t found =
        find_lattice_recursive(t, t.root_id(), name, last_lattice, use_name);
    if (found != ryml::NONE) return found;
    if (!name.empty()) return ryml::NONE;

    if (!use_name.empty()) return find_lattice(t, use_name);

    return last_lattice;
}

/**
 * Create the expanded lattice. Starts with the combined lattice, and expands
 * the lattice with the following priorities:
 *  1. If `root_lattice` != null, then expand the lattice called `root_lattice`
 *  2. If `root_lattice` == null, expand the lattice specified in the last `use`
 * statement.
 *  3. If no use statement is present, expand the lattice that occurs last in
 * the file. Last expansion performs the following:
 * 1. Substitute scalar elements with their full definition, if defined in the
 * file outside the lattice.
 * 2. Beamlines that contain "repeat: n" have their contents repeated |n| times,
 * in reverse order when n is negative.
 * 3. Elements that contain "inherit: ancestor" have the contents of ancestor
 * copied into element.
 */
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

// Shortest decimal string that round-trips back to `v` (so integers stay
// integer-looking and no precision is lost).
static std::string format_double(double v) {
    char buf[64];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) return std::string(buf);
    }
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

// ------------------------------------------------------------
// Tree-writing helpers, shared by the controller pass and the element
// bookkeeper: both fill parameters into element definitions, creating the
// enclosing parameter group when it is not already there.
// ------------------------------------------------------------

// A keyed sequence child of `parent`, created (empty) if absent.
static size_t find_or_add_seq_child(ryml::Tree& t, size_t parent,
                                    const char* key) {
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c != ryml::NONE) return c;
    ensure_capacity(t, 2);
    c = t.append_child(parent);
    t.ref(c) |= ryml::KEY | ryml::SEQ;
    t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    return c;
}

// A keyed map child of `parent`, created (empty) if absent. An existing scalar
// placeholder of the same key is converted to a map in place.
static size_t find_or_add_map_child(ryml::Tree& t, size_t parent,
                                    const char* key) {
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c != ryml::NONE) {
        if (!t.is_map(c)) {
            t.change_type(c, ryml::KEYMAP);
            t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
        }
        return c;
    }
    ensure_capacity(t, 2);
    c = t.append_child(parent);
    t.ref(c) |= ryml::KEY | ryml::MAP;
    t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    return c;
}

// Set (or create) a keyed scalar child to the shortest round-tripping decimal.
static void set_num_child(ryml::Tree& t, size_t parent, const char* key,
                          double v) {
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c == ryml::NONE) {
        ensure_capacity(t, 2);
        c = t.append_child(parent);
        t.ref(c) |= ryml::KEY | ryml::VAL;
        t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    }
    t.set_val(c, t.to_arena(ryml::to_csubstr(format_double(v))));
}

// Overwrite a scalar value node in place with a number.
static void set_scalar_num(ryml::Tree& t, size_t node, double v) {
    t.set_val(node, t.to_arena(ryml::to_csubstr(format_double(v))));
}

// Set (or create) a keyed scalar child to a string. Double-quoted so a leading
// '#' in a species name survives YAML's comment rule on re-emit.
static void set_str_child(ryml::Tree& t, size_t parent, const char* key,
                          const std::string& v) {
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c == ryml::NONE) {
        ensure_capacity(t, 2);
        c = t.append_child(parent);
        t.ref(c) |= ryml::KEY | ryml::VAL;
        t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    }
    t.set_val(c, t.to_arena(ryml::to_csubstr(v)));
    t.set_val_style(c, ryml::VAL_DQUO);
}

// Set (or create) a keyed scalar child to a bare (unquoted) token. Used for
// enum and boolean defaults (e.g. `cavity_type: STANDING_WAVE`,
// `aperture_active: true`), which are plain YAML scalars, not strings.
static void set_plain_child(ryml::Tree& t, size_t parent, const char* key,
                            const char* v) {
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c == ryml::NONE) {
        ensure_capacity(t, 2);
        c = t.append_child(parent);
        t.ref(c) |= ryml::KEY | ryml::VAL;
        t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    }
    t.set_val(c, t.to_arena(ryml::to_csubstr(v)));
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
// parameters, the set
// of one member of the group nullifies previous settings of all other members
// of the group." Nullifying is what keeps the family from being flagged as
// inconsistent when the bookkeeper derives it: whoever writes last states the
// quantity, and the earlier statements of it are gone. The rule holds for every
// writer that acts on a parameter from outside the element definition -- `set`
// commands, before and after expansion, and ABSOLUTE controllers alike. Two
// members written inside one element definition are a different matter: neither
// is "previous", so that stays an inconsistency for the bookkeeper to report.
// ------------------------------------------------------------

// The family `key` belongs to within `group`, including `key` itself; empty for
// a parameter that stands alone.
//
// This is also what tells a value expression that a parameter is *not yet
// computable* rather than zero: before the bookkeeper has run, an absent
// parameter with a written family member is one whose value is still to be
// derived (lattice-construction.md, s:lattice.expand).
static std::set<std::string> linked_family(const std::string& group,
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
static void nullify_family(ryml::Tree& t, size_t ele,
                           const std::vector<std::string>& path,
                           std::map<size_t, size_t>& prov) {
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
        erase_prov_subtree(t, node, prov);
        t.remove(node);
    }
}

// True if `node` is a map defining a `kind: Controller` element.
static bool is_controller(const ryml::Tree& t, size_t node) {
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
            std::string k(t.val(kind).str, t.val(kind).len);
            if (k == "constant" || k == "variable") {
                size_t val = t.find_child(node, ryml::to_csubstr("value"));
                if (val != ryml::NONE && t.has_val(val)) {
                    std::string name(t.key(node).str, t.key(node).len);
                    std::string text(t.val(val).str, t.val(val).len);
                    defs.emplace(name, text);
                    if (all) all->emplace_back(name, text);
                }
            }
        }
    }

    // Compact form: a `constants:`/`variables:` block. The standard writes it
    // as a sequence of single-key maps (`- const_a: ...`), but the plain-map
    // form (`const_a: ...` directly under the key) is also accepted.
    if (t.has_key(node)) {
        std::string k(t.key(node).str, t.key(node).len);
        if (k == "constants" || k == "variables") {
            auto emit = [&](size_t kv) {
                if (!t.has_key(kv) || !t.has_val(kv)) return;
                std::string name(t.key(kv).str, t.key(kv).len);
                std::string text(t.val(kv).str, t.val(kv).len);
                defs.emplace(name, text);
                if (all) all->emplace_back(name, text);
            };
            if (t.is_map(node)) {
                for (size_t kv = t.first_child(node); kv != ryml::NONE;
                     kv = t.next_sibling(kv))
                    emit(kv);
            } else if (t.is_seq(node)) {
                for (size_t el = t.first_child(node); el != ryml::NONE;
                     el = t.next_sibling(el)) {
                    if (!t.is_map(el)) continue;
                    for (size_t kv = t.first_child(el); kv != ryml::NONE;
                         kv = t.next_sibling(kv))
                        emit(kv);
                }
            }
        }
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

// Replaces every evaluable scalar value in the subtree with its numeric value.
// Controller and `set` subtrees are skipped: their expressions are evaluated
// against a scope of their own, by evaluate_controllers and execute_set.
// A value that looks like an expression but cannot be evaluated is recorded in
// `problems`.
static void substitute_values(ryml::Tree& t, size_t node,
                              const pals::SymbolLookup& resolve,
                              const pals::SpeciesLookup& species,
                              ProblemList& problems) {
    if (node == ryml::NONE || is_controller(t, node) || is_set_node(t, node))
        return;

    if (t.has_val(node)) {
        bool skip = false;
        if (t.has_key(node)) {
            std::string k(t.key(node).str, t.key(node).len);
            skip = non_expr_keys().count(k) != 0;
        } else {
            // Bare sequence element: it has no key of its own, so the decision
            // belongs to the key of the list it sits in -- the entries of
            // `notes` are prose for the same reason the key is. Beamline
            // `line:` name references are skipped the same way.
            size_t p = t.parent(node);
            if (p != ryml::NONE && t.has_key(p)) {
                std::string pk(t.key(p).str, t.key(p).len);
                skip = pk == "line" || non_expr_keys().count(pk) != 0;
            }
        }
        if (!skip) {
            bool was_expr = false;
            std::string body = strip_expr_wrapper(
                std::string(t.val(node).str, t.val(node).len), was_expr);
            pals::EvalOutcome r = pals::eval_expression(body, resolve, species);
            std::string sp;
            if (r.ok) {
                t.set_val(node,
                          t.to_arena(ryml::to_csubstr(format_double(r.value))));
            } else if (!r.deferred && species && species(body, sp)) {
                // A bare identifier that names a species-valued constant or
                // variable (e.g. `species_ref: species` with `species: "#3He"`)
                // is replaced by its species-name string. Double-quoted so a
                // leading `#` survives YAML's comment rule on re-emit.
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
    }

    for (size_t c = t.first_child(node); c != ryml::NONE;
         c = t.next_sibling(c))
        substitute_values(t, c, resolve, species, problems);
}

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

// Iterates the `variables` of a controller, in both the documented map form
// (`cur1: 0.023`) and the compact seq-of-single-key-maps form, invoking `emit`
// with each (name, value-node) pair. A variable written with no value at all
// (`cur1:`) is emitted too: its value is the default, zero.
template <typename F>
static void for_each_ctrl_var(const ryml::Tree& t, size_t vars, F&& emit) {
    auto visit = [&](size_t kv) {
        if (t.has_key(kv) && !t.is_map(kv) && !t.is_seq(kv))
            emit(std::string(t.key(kv).str, t.key(kv).len), kv);
    };
    if (vars == ryml::NONE) return;
    if (t.is_map(vars)) {
        for (size_t kv = t.first_child(vars); kv != ryml::NONE;
             kv = t.next_sibling(kv))
            visit(kv);
    } else if (t.is_seq(vars)) {
        for (size_t el = t.first_child(vars); el != ryml::NONE;
             el = t.next_sibling(el))
            for (size_t kv = t.first_child(el); kv != ryml::NONE;
                 kv = t.next_sibling(kv))
                visit(kv);
    }
}

// Trimmed scalar value of a node; "" when the node holds nothing. A YAML null
// (`~`, `null`) reads as "no value given" the same as an empty scalar does.
static std::string scalar_text(const ryml::Tree& t, size_t node) {
    if (node == ryml::NONE || !t.has_val(node)) return "";
    std::string v(t.val(node).str, t.val(node).len);
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    v = v.substr(a, v.find_last_not_of(" \t\r\n") - a + 1);
    if (v == "~" || v == "null") return "";
    return v;
}

// The identifiers appearing in an expression: maximal runs of name characters
// that do not start with a digit. Used to spot a name an expression is not
// allowed to use, which a plain substring search would get wrong (`c_light`
// does not reference a variable named `light`).
static std::vector<std::string> expression_identifiers(const std::string& s) {
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

// Reads every `kind: Controller` in the tree into `ctrls` (and their variables
// into the flat `vars` list), materializing the `control_type` default and
// rejecting the expressions the standard does not allow.
static void collect_controller_defs(ryml::Tree& t, std::vector<Ctrl>& ctrls,
                                    std::vector<CtrlVar>& vars,
                                    ProblemList& problems) {
    std::vector<size_t> nodes;
    collect_controllers(t, t.root_id(), nodes);

    // Neither an initial value nor a control expression may reach outside its
    // own controller (parameter-values-and-controllers.md, s:controller). Every such reference --
    // a lattice parameter, another controller's variable -- is spelled with
    // `>`, so one test covers both, and it is what keeps evaluation order
    // decidable from the `controls` hierarchy alone.
    auto reject_refs = [&problems](const std::string& what,
                                   const std::string& text) {
        if (text.find('>') == std::string::npos) return false;
        add_problem(problems,
                    what + " may not reference a lattice parameter or another "
                           "controller's variable: " + text);
        return true;
    };

    for (size_t node : nodes) {
        Ctrl c;
        c.node = node;
        if (t.has_key(node)) c.name.assign(t.key(node).str, t.key(node).len);

        std::string ct = scalar_text(t, t.find_child(node, ryml::to_csubstr(
                                                               "control_type")));
        if (ct.empty()) {
            set_plain_child(t, node, "control_type", "ABSOLUTE");
        } else if (ct == "RELATIVE") {
            c.absolute = false;
        } else if (ct != "ABSOLUTE") {
            add_problem(problems, "controller '" + c.name +
                                      "': control_type must be ABSOLUTE or "
                                      "RELATIVE, not " + ct);
        }

        size_t vnode = t.find_child(node, ryml::to_csubstr("variables"));
        for_each_ctrl_var(t, vnode, [&](const std::string& vname, size_t vn) {
            CtrlVar v;
            v.node = vn;
            v.ctrl = ctrls.size();
            v.name = vname;
            v.text = scalar_text(t, vn);
            if (reject_refs("controller '" + c.name + "' variable '" + vname +
                                "'",
                            v.text))
                v.text.clear();
            c.vars.push_back(vars.size());
            vars.push_back(std::move(v));
        });

        // An initial value may not name a variable at all, not even one of this
        // controller's own: it is a constant expression, so no variable has to
        // be evaluated before any other. Only the control `expression`s use the
        // variables. Checked here, once every name of this controller is known,
        // so it does not matter which order the variables are written in.
        std::set<std::string> vnames;
        for (size_t vi : c.vars) vnames.insert(vars[vi].name);
        for (size_t vi : c.vars) {
            for (const std::string& id : expression_identifiers(vars[vi].text)) {
                if (!vnames.count(id)) continue;
                add_problem(problems, "controller '" + c.name + "' variable '" +
                                          vars[vi].name +
                                          "': an initial value may not "
                                          "reference the variable '" + id + "'");
                vars[vi].text.clear();
                break;
            }
        }

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
                    add_problem(problems,
                                "controller '" + c.name + "': controls entry '" +
                                    cc.param + "' has no `expression`");
                    continue;
                }
                if (reject_refs("controller '" + c.name + "' control expression",
                                cc.text))
                    continue;
                c.controls.push_back(std::move(cc));
            }
        }
        ctrls.push_back(std::move(c));
    }
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
    for (size_t ci = 0; ci < ctrls.size(); ++ci)
        for (const CtrlControl& c : ctrls[ci].controls)
            if (c.target_ctrl != SIZE_MAX) {
                below[ci].push_back(c.target_ctrl);
                ++indeg[c.target_ctrl];
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
};

// Evaluates the controllers and applies them to the expanded lattice.
//
// Each controller's `variables` are a symbol table scoped to that controller:
// an initial value may use the controller's own variables, the built-in and
// user constants, and nothing else. A variable written with no value is zero.
// Controllers are walked from the top of the hierarchy downwards, so a variable
// driven from above already holds its driving value by the time the controller
// owning it is evaluated. Every control `expression` is then computed with its
// controller's table and the number written back into the control entry, and
// the ABSOLUTE controllers are applied to the lattice parameters they drive.
//
// `lat_node` is the expanded lattice; parameter targets are matched inside it,
// which is what keeps a controller from also driving the unexpanded element
// definitions still sitting in `facility`. Passing ryml::NONE evaluates the
// controllers without applying them.
static void evaluate_controllers(ryml::Tree& t, size_t lat_node,
                                 const pals::SymbolLookup& global_resolve,
                                 const pals::SpeciesLookup& species,
                                 std::map<size_t, size_t>& prov,
                                 ProblemList& problems) {
    std::vector<Ctrl> ctrls;
    std::vector<CtrlVar> vars;
    collect_controller_defs(t, ctrls, vars, problems);
    if (ctrls.empty()) return;

    std::map<std::string, size_t> by_name;
    for (size_t ci = 0; ci < ctrls.size(); ++ci) by_name[ctrls[ci].name] = ci;

    // (controller, variable) -> index into `vars`.
    std::map<std::pair<size_t, std::string>, size_t> var_of;
    for (size_t vi = 0; vi < vars.size(); ++vi)
        var_of[{vars[vi].ctrl, vars[vi].name}] = vi;

    // Split each `parameter` target into the two things it can name. A target
    // whose part before the `>` is a controller name is a controller variable
    // (`ps27>cur1`); anything else is a lattice parameter.
    for (Ctrl& c : ctrls) {
        for (CtrlControl& cc : c.controls) {
            size_t gt = cc.param.find('>');
            if (gt == std::string::npos) continue;
            auto it = by_name.find(cc.param.substr(0, gt));
            if (it == by_name.end()) continue;
            std::string var = cc.param.substr(gt + 1);
            if (!var_of.count({it->second, var})) {
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

    std::vector<size_t> order = controller_order(ctrls, problems);

    // Symbol tables, filled in as each controller is reached.
    std::vector<std::map<std::string, double>> locals(ctrls.size());
    auto resolver_for = [&locals, &global_resolve](size_t ci) {
        return pals::SymbolLookup(
            [ci, &locals, &global_resolve](const std::string& name,
                                           double& out) -> bool {
                auto it = locals[ci].find(name);
                if (it != locals[ci].end()) {
                    out = it->second;
                    return true;
                }
                return global_resolve ? global_resolve(name, out) : false;
            });
    };

    std::map<std::pair<size_t, std::string>, CtrlTarget> targets;

    for (size_t ci : order) {
        Ctrl& c = ctrls[ci];
        pals::SymbolLookup res = resolver_for(ci);

        // Build this controller's symbol table. A driven variable takes the
        // value the controllers above set -- that is what it replaced its own
        // initial value with. Every other initial value is a constant
        // expression (no variable may appear in one), so a single pass in any
        // order settles the table.
        for (size_t vi : c.vars) {
            CtrlVar& v = vars[vi];
            if (v.driven) {
                locals[ci][v.name] = v.driven_value;
                set_scalar_num(t, v.node, v.driven_value);
                v.done = true;
                continue;
            }
            if (v.text.empty()) {  // no value given: the default is zero
                locals[ci][v.name] = 0.0;
                set_scalar_num(t, v.node, 0.0);
                v.done = true;
                continue;
            }
            bool was_expr = false;
            std::string body = strip_expr_wrapper(v.text, was_expr);
            // `global_resolve`, not `res`: an initial value sees the built-in
            // and user constants, never the variables.
            pals::EvalOutcome r =
                pals::eval_expression(body, global_resolve, species);
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
            pals::EvalOutcome r = pals::eval_expression(body, res, species);
            if (r.ok) {
                cc.ok = true;
                cc.value = r.value;
                set_scalar_num(t, cc.expr_node, r.value);
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
            CtrlVar& tv = vars[var_of[{cc.target_ctrl, cc.target_var}]];
            if (!tv.driven) {
                tv.driven = true;
                tv.driven_value = 0.0;
            }
            tv.driven_value += cc.value;  // several ABSOLUTE controllers sum
        }
    }

    if (lat_node == ryml::NONE) return;

    // Gather what drives each lattice parameter. Targets are keyed on the
    // element and the path rather than on a parameter node, so a parameter the
    // element does not carry yet is still recognised as the same target.
    // Walked in evaluation order, and each `controls` entry numbered, so that
    // "previous" has a meaning when two of them name one interdependent family.
    size_t seq = 0;
    for (size_t ci : order) {
        const Ctrl& c = ctrls[ci];
        for (const CtrlControl& cc : c.controls) {
            ++seq;
            if (cc.target_ctrl != SIZE_MAX) continue;
            ElementMatches m = match_element_parameters(t, lat_node, cc.param);
            if (!m.valid) {
                add_problem(problems, "controller '" + c.name +
                                          "': malformed parameter target '" +
                                          cc.param + "'");
                continue;
            }
            bool named = m.has_param;
            for (const std::string& p : m.path)
                if (p.empty()) named = false;
            if (!named) {
                add_problem(problems, "controller '" + c.name + "': target '" +
                                          cc.param +
                                          "' names no element parameter");
                continue;
            }
            if (m.elements.empty()) {
                add_problem(problems, "controller '" + c.name + "': target '" +
                                          cc.param +
                                          "' matches nothing in the expanded "
                                          "lattice");
                continue;
            }
            std::string path;
            for (const std::string& p : m.path)
                path += (path.empty() ? "" : ".") + p;
            for (size_t def : m.elements) {
                CtrlTarget& tg = targets[{def, path}];
                tg.seq = seq;
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

    // Applied in the order the controls were reached, not in the map's key
    // order: a later write nullifies the family members an earlier one stated,
    // so which one comes last decides what the parameter ends up saying.
    std::vector<decltype(targets)::const_iterator> in_order;
    in_order.reserve(targets.size());
    for (auto it = targets.begin(); it != targets.end(); ++it)
        in_order.push_back(it);
    std::stable_sort(in_order.begin(), in_order.end(),
                     [](const auto& a, const auto& b) {
                         return a->second.seq < b->second.seq;
                     });

    for (const auto& it : in_order) {
        const auto& kv = *it;
        size_t def = kv.first.first;
        const CtrlTarget& tg = kv.second;
        std::vector<std::string> path = split_dots(kv.first.second);
        std::string where = std::string(t.key(def).str, t.key(def).len) + ">" +
                            kv.first.second;

        // "A given lattice parameter may not be assigned a delayed evaluation
        // expression and be controlled by a controller." This runs before
        // substitute_values, while the `expr(...)` wrapper is still there to see.
        size_t existing = resolve_param_path(t, def, path);
        if (existing != ryml::NONE && t.has_val(existing)) {
            bool was_expr = false;
            strip_expr_wrapper(std::string(t.val(existing).str,
                                           t.val(existing).len),
                               was_expr);
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
        // The controller states this member of its family; what the element
        // definition, or a control reached earlier, said through another member
        // is nullified rather than left to be flagged as inconsistent.
        nullify_family(t, def, path, prov);
    }
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
static void make_expression_context(const ryml::Tree& t,
                                    pals::SymbolLookup& resolve_out,
                                    pals::SpeciesLookup& species_out) {
    auto defs = std::make_shared<std::map<std::string, std::string>>();
    collect_defs(t, t.root_id(), *defs);

    // Element name -> definition map, so expressions may reference another
    // element's parameter via `element>group. ... .param`.
    auto emap = std::make_shared<std::map<std::string, size_t>>();
    make_ele_map(*emap, t, t.root_id());

    // Resolves a symbol whose value is a species-name string (e.g.
    // `species: "#3He"`), so a particle-data function may take it by name:
    // `mass_of(species)`. The stored value is returned verbatim (trimmed, with
    // any surrounding quotes stripped); the expression evaluator validates it.
    pals::SpeciesLookup species = [defs](const std::string& name,
                                         std::string& out) -> bool {
        auto di = defs->find(name);
        if (di == defs->end()) return false;
        std::string v = di->second;
        size_t a = v.find_first_not_of(" \t\r\n");
        size_t b = v.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return false;
        v = v.substr(a, b - a + 1);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') &&
            v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        out = v;
        return true;
    };

    // Lazily evaluate user symbols on demand, memoizing results and guarding
    // against reference cycles. A symbol whose defining expression is itself
    // unresolvable is reported as unknown.
    auto cache = std::make_shared<std::map<std::string, double>>();
    auto active = std::make_shared<std::set<std::string>>();
    std::shared_ptr<pals::SymbolLookup> resolve =
        std::make_shared<pals::SymbolLookup>();
    const ryml::Tree* tp = &t;
    *resolve = [defs, tp, emap, cache, active, resolve, species](
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
            body = std::string(tp->val(vn).str, tp->val(vn).len);
        } else {
            auto di = defs->find(name);
            if (di == defs->end()) return false;
            body = di->second;
        }
        if (!active->insert(name).second) return false;  // cycle
        bool was_expr = false;
        body = strip_expr_wrapper(body, was_expr);
        pals::EvalOutcome r = pals::eval_expression(body, *resolve, species);
        active->erase(name);
        if (!r.ok) return false;
        (*cache)[name] = r.value;
        out = r.value;
        return true;
    };

    resolve_out = *resolve;
    species_out = species;
}

// Check the constants and variables (parameter-values-and-controllers.md,
// s:constants): a name defined more than once must be given the same value
// each time, and no definition may depend on itself, directly or through
// others.
static void check_definitions(const ryml::Tree& t, ProblemList& problems) {
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
    pals::SymbolLookup resolve;
    pals::SpeciesLookup species;
    make_expression_context(t, resolve, species);
    std::set<std::string> said;
    for (const auto& d : all) {
        const std::string& first = defs[d.first];
        if (d.second == first || said.count(d.first)) continue;
        bool w1 = false, w2 = false;
        pals::EvalOutcome a =
            pals::eval_expression(strip_expr_wrapper(first, w1), resolve, species);
        pals::EvalOutcome b = pals::eval_expression(
            strip_expr_wrapper(d.second, w2), resolve, species);
        if (a.ok && b.ok && a.value == b.value) continue;
        said.insert(d.first);
        add_problem(problems,
                    "'" + d.first + "' is defined more than once with different "
                    "values ('" + first + "' and '" + d.second + "')",
                    d.first);
    }
}

// Evaluates all expressions in the (already expanded) tree in place, and
// applies the controllers to `lat_node`. Records anything that could not be
// evaluated in `problems`.
static void evaluate_expressions(ryml::Tree& t, size_t lat_node,
                                 std::map<size_t, size_t>& prov,
                                 ProblemList& problems) {
    pals::SymbolLookup resolve;
    pals::SpeciesLookup species;
    make_expression_context(t, resolve, species);

    evaluate_controllers(t, lat_node, resolve, species, prov, problems);
    substitute_values(t, t.root_id(), resolve, species, problems);
}

// ============================================================
// ELEMENT BOOKKEEPING
// ============================================================
//
// Once every expression has been reduced to a number, each branch is walked
// element-by-element from its BeginningEle to compute the output parameters that
// depend on where an element sits in the branch: the reference parameters
// (species / energy / momentum / time), the floor placement, the s-position,
// and the field-dependent parameters (normalized <-> unnormalized multipole and
// bend strengths). See the "Lattice Expansion" step in
// pals/source/lattice-construction.md; the floor math (Eqs. wws etc.) lives in
// pals_floor.cpp.
//
// All ReferenceP / FloorP values describe the *upstream* end of their element.
// The bookkeeper persists each element's results into the tree before moving on,
// so _element_bookkeeper reads the previous element's stored parameters and
// "propagates" them onto the current one, exactly as the standard describes.

// Parse the whole of `s` (bar surrounding whitespace) as a finite double. Values
// in the expanded tree are already evaluated to plain numbers by this point.
static bool parse_num(const std::string& s, double& out) {
    const char* p = s.c_str();
    char* end = nullptr;
    double d = std::strtod(p, &end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') ++end;
    if (end != p && *end == '\0' && std::isfinite(d)) {
        out = d;
        return true;
    }
    return false;
}

// Numeric value of a keyed child, if present and numeric.
static bool get_num_child(const ryml::Tree& t, size_t parent, const char* key,
                          double& out) {
    if (parent == ryml::NONE) return false;
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c == ryml::NONE || !t.has_val(c)) return false;
    return parse_num(std::string(t.val(c).str, t.val(c).len), out);
}

// String value of a keyed child, trimmed and with any surrounding quotes
// stripped; false (and `out` untouched) if absent or empty.
static bool get_str_child(const ryml::Tree& t, size_t parent, const char* key,
                          std::string& out) {
    if (parent == ryml::NONE) return false;
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c == ryml::NONE || !t.has_val(c)) return false;
    std::string v(t.val(c).str, t.val(c).len);
    size_t a = v.find_first_not_of(" \t\r\n");
    size_t b = v.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    v = v.substr(a, b - a + 1);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') &&
        v.back() == v.front())
        v = v.substr(1, v.size() - 2);
    if (v.empty()) return false;
    out = v;
    return true;
}

// ============================================================
// SET COMMANDS, facility AND post_expansion
// ============================================================
//
// A `set` (parameter-values-and-controllers.md, s:set) writes a value into
// every parameter its `parameter` name-matching string selects:
//
//   - set:
//       parameter: B1.*>BendP.e1
//       value: 2*PARAMETER + atan(SELF.BendP.g_ref)
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

// The keyed list `key` (`facility`, `post_expansion`) of the PALS node, or
// ryml::NONE. Includes have already been spliced into it in place, so it holds
// every node of every file, in order.
static size_t find_pals_list(const ryml::Tree& t, const char* key) {
    size_t pals = t.find_child(t.root_id(), ryml::to_csubstr("PALS"));
    if (pals == ryml::NONE) return ryml::NONE;
    size_t l = t.find_child(pals, ryml::to_csubstr(key));
    return (l != ryml::NONE && t.is_seq(l)) ? l : ryml::NONE;
}

// The entries of one of the PALS lists, in order.
static std::vector<size_t> pals_list_entries(const ryml::Tree& t,
                                             const char* key) {
    std::vector<size_t> out;
    size_t l = find_pals_list(t, key);
    if (l == ryml::NONE) return out;
    for (size_t e = t.first_child(l); e != ryml::NONE; e = t.next_sibling(e))
        out.push_back(e);
    return out;
}

// The keyed child of a list entry (the entry is an anonymous wrapper map
// holding one keyed node), or ryml::NONE for a bare scalar entry.
static size_t entry_content(const ryml::Tree& t, size_t entry) {
    if (entry == ryml::NONE || !t.is_map(entry)) return ryml::NONE;
    size_t c = t.first_child(entry);
    return (c != ryml::NONE && t.has_key(c)) ? c : ryml::NONE;
}

// Everything a `set` needs to know about where it is writing: the element, the
// group node holding the parameter (ryml::NONE until it is created), the
// parameter key, and the group name for the family lookup.
struct SetTarget {
    size_t ele = ryml::NONE;
    std::vector<std::string> path;
    std::string group;  // "" for an ungrouped parameter such as `length`
    std::string key;
};

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

// Reads the value a `set` expression sees for a parameter: the written value if
// there is one, otherwise zero (an unwritten parameter of a known element is
// zero, per s:lattice.expand). `pending` reports the third case -- the value is
// still to be derived, so reading it is an error.
static bool read_set_operand(const ryml::Tree& t, size_t ele,
                             const std::vector<std::string>& path,
                             const pals::SymbolLookup& resolve,
                             const pals::SpeciesLookup& species, bool pre,
                             double& out, bool& pending) {
    pending = false;
    if (ele == ryml::NONE || path.empty()) return false;
    size_t node = resolve_param_path(t, ele, path);
    if (node != ryml::NONE && t.has_val(node)) {
        bool was_expr = false;
        std::string body = strip_expr_wrapper(
            std::string(t.val(node).str, t.val(node).len), was_expr);
        pals::EvalOutcome r = pals::eval_expression(body, resolve, species);
        if (!r.ok) return false;
        out = r.value;
        return true;
    }
    if (pre && awaits_derivation(t, ele, path)) {
        pending = true;
        return false;
    }
    out = 0.0;
    return true;
}

// One `set` command, read out of the tree.
struct SetCommand {
    std::string param;   // `parameter`: the name-matching target
    std::string value;   // `value`: the expression to write
    double abs_error = 0.0;
    double rel_error = 0.0;
    bool has_error = false;
};

// Execute one `set` against the elements `matches` selected. `pre` distinguishes
// a pre-expansion set (acting on definitions, where a derived value is not yet
// available) from a post-expansion one (acting on the expanded lattice, where
// writing a parameter makes its family stale).
//
// `written`, when given, collects one target per parameter actually written --
// what the `expanded` tree needs in order to tell a post-expansion set's input
// apart from a value the bookkeeper derived from it (see AuthoredParams).
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
    std::string name(t.val(node).str, t.val(node).len);
    t.to_map(node);  // the seq entry becomes the anonymous wrapper map
    size_t def = t.append_child(node);
    t.to_map(def, t.to_arena(ryml::to_csubstr(name)));
    return def;
}

static void execute_set(ryml::Tree& t, const SetCommand& cmd,
                        const ElementMatches& matches, bool pre,
                        const pals::SymbolLookup& resolve,
                        const pals::SpeciesLookup& species,
                        std::map<size_t, size_t>& prov, ProblemList& problems,
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

    // Only a pre-expansion set needs the element map, to spot a reference to a
    // parameter whose value has still to be derived.
    std::map<std::string, size_t> emap;
    if (pre) make_ele_map(emap, t, t.root_id());

    for (size_t match : matches.elements) {
        size_t ele = materialize_line_reference(t, match);
        SetTarget tg = make_target(ele, matches.path);
        std::string ename(t.key(ele).str, t.key(ele).len);
        std::string where = what + " on '" + ename + "'";

        // `PARAMETER` is the value being replaced; `SELF.<path>` reaches the
        // rest of the element. Both are scoped to this one element, so the
        // resolver is rebuilt for each.
        bool pending = false;
        std::string pending_name;
        pals::SymbolLookup scoped = [&](const std::string& name,
                                        double& out) -> bool {
            if (name == "PARAMETER") {
                bool p = false;
                bool ok = read_set_operand(t, ele, tg.path, resolve, species,
                                           pre, out, p);
                if (p) {
                    pending = true;
                    pending_name = matches.path.back();
                }
                return ok;
            }
            if (name.compare(0, 5, "SELF.") == 0) {
                std::vector<std::string> path = split_dots(name.substr(5));
                bool p = false;
                bool ok =
                    read_set_operand(t, ele, path, resolve, species, pre, out, p);
                if (p) {
                    pending = true;
                    pending_name = name;
                }
                return ok;
            }
            // An `element>path` reference reads by the same rules: the written
            // value, else zero, else -- when a family member is written, so the
            // value is still to be derived -- an error.
            size_t gt = name.find('>');
            if (pre && gt != std::string::npos) {
                auto it = emap.find(name.substr(0, gt));
                if (it != emap.end()) {
                    std::vector<std::string> ref =
                        split_dots(name.substr(gt + 1));
                    bool p = false;
                    bool ok = read_set_operand(t, it->second, ref, resolve,
                                               species, pre, out, p);
                    if (p) {
                        pending = true;
                        pending_name = name;
                    }
                    return ok;
                }
            }
            return resolve ? resolve(name, out) : false;
        };

        bool was_expr = false;
        std::string body = strip_expr_wrapper(cmd.value, was_expr);
        pals::EvalOutcome r = pals::eval_expression(body, scoped, species);
        if (pending) {
            add_problem(problems, where + ": '" + pending_name +
                                      "' has no value yet -- it is derived "
                                      "during lattice expansion");
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
        set_num_child(t, parent, tg.key.c_str(), r.value);
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
    std::string key(t.key(c).str, t.key(c).len);

    if (key == "set") {
        SetCommand cmd;
        if (read_set_command(t, c, cmd, problems)) out.push_back(std::move(cmd));
        return out;
    }
    if (key != "sets") return out;

    // Compact form: a list of `target: value` pairs (written as a sequence of
    // single-key maps, or as a plain map).
    auto emit = [&](size_t kv) {
        if (!t.has_key(kv) || !t.has_val(kv)) return;
        SetCommand cmd;
        cmd.param.assign(t.key(kv).str, t.key(kv).len);
        cmd.value.assign(t.val(kv).str, t.val(kv).len);
        out.push_back(std::move(cmd));
    };
    if (t.is_map(c)) {
        for (size_t kv = t.first_child(c); kv != ryml::NONE;
             kv = t.next_sibling(kv))
            emit(kv);
    } else if (t.is_seq(c)) {
        for (size_t el = t.first_child(c); el != ryml::NONE;
             el = t.next_sibling(el))
            for (size_t kv = t.first_child(el); kv != ryml::NONE;
                 kv = t.next_sibling(kv))
                emit(kv);
    }
    return out;
}

// Run the `set` commands of the `facility` list, in list order. Each acts on the
// element definitions that precede it, which is why the entries are walked
// rather than the tree: `Q2` defined after a set is not touched by it.
static void run_pre_expansion_sets(ryml::Tree& t,
                                   const std::vector<size_t>& entries,
                                   std::map<size_t, size_t>& prov,
                                   ProblemList& problems) {
    std::vector<std::vector<SetCommand>> cmds(entries.size());
    bool any = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        cmds[i] = entry_set_commands(t, entries[i], problems);
        if (!cmds[i].empty()) any = true;
    }
    if (!any) return;  // the usual case: no sets at all

    std::vector<size_t> defined;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (cmds[i].empty()) {
            defined.push_back(entries[i]);
            continue;
        }
        // A fresh context per command: each set writes to the tree the next one
        // reads, so a memoized value from before it would be stale.
        for (const SetCommand& cmd : cmds[i]) {
            pals::SymbolLookup resolve;
            pals::SpeciesLookup species;
            make_expression_context(t, resolve, species);
            execute_set(t, cmd,
                        match_definition_parameters(t, defined, cmd.param), true,
                        resolve, species, prov, problems);
        }
    }
}

// Run the `set` commands of the `post_expansion` list against the expanded
// lattice. Order still matters -- a later set sees what an earlier one wrote --
// but every set sees the whole lattice, since it has already been built.
static void run_post_expansion_sets(ryml::Tree& t, size_t lat_node,
                                    const std::vector<size_t>& entries,
                                    std::map<size_t, size_t>& prov,
                                    ProblemList& problems,
                                    std::vector<SetTarget>* written = nullptr) {
    if (lat_node == ryml::NONE) return;
    for (size_t e : entries) {
        for (const SetCommand& cmd : entry_set_commands(t, e, problems)) {
            pals::SymbolLookup resolve;
            pals::SpeciesLookup species;
            make_expression_context(t, resolve, species);
            execute_set(t, cmd,
                        match_element_parameters(t, lat_node, cmd.param), false,
                        resolve, species, prov, problems, written);
        }
    }
}

// Reference parameters at one end of an element (ReferenceP). Energy and
// momentum are related through the reference mass; a flag records which of them
// is known so complete_energy() can fill the other.
struct RefState {
    std::string species;
    double E_tot = 0.0;  // [eV] total energy
    double pc = 0.0;     // [eV] momentum * c
    double time = 0.0;   // [s]
    bool has_species = false;
    bool has_E = false;
    bool has_pc = false;
};

// Fill in whichever of E_tot / pc is missing from the other, using the reference
// species mass (E^2 = (pc)^2 + (m c^2)^2, all in eV). A no-op if the species is
// unknown, both are set, or neither is.
static void complete_energy(RefState& r) {
    if (!r.has_species) return;
    double m;
    try {
        m = apc::mass_of(r.species);
    } catch (...) {
        return;  // unknown species name; leave energy/momentum as-is
    }
    if (r.has_E && !r.has_pc) {
        double v = r.E_tot * r.E_tot - m * m;
        if (v >= 0.0) {
            r.pc = std::sqrt(v);
            r.has_pc = true;
        }
    } else if (r.has_pc && !r.has_E) {
        r.E_tot = std::sqrt(r.pc * r.pc + m * m);
        r.has_E = true;
    }
}

// Read the ReferenceP group of an element into a RefState (its upstream values).
static RefState read_ref(const ryml::Tree& t, size_t ele) {
    RefState r;
    size_t rp = t.find_child(ele, ryml::to_csubstr("ReferenceP"));
    if (rp == ryml::NONE) return r;
    r.has_species = get_str_child(t, rp, "species_ref", r.species);
    r.has_E = get_num_child(t, rp, "E_tot_ref", r.E_tot);
    r.has_pc = get_num_child(t, rp, "pc_ref", r.pc);
    get_num_child(t, rp, "time_ref", r.time);  // defaults to 0
    return r;
}

// Write a RefState back into an element's ReferenceP group (creating it).
static void write_ref(ryml::Tree& t, size_t ele, const RefState& r) {
    size_t rp = find_or_add_map_child(t, ele, "ReferenceP");
    if (r.has_species) set_str_child(t, rp, "species_ref", r.species);
    if (r.has_E) set_num_child(t, rp, "E_tot_ref", r.E_tot);
    if (r.has_pc) set_num_child(t, rp, "pc_ref", r.pc);
    set_num_child(t, rp, "time_ref", r.time);
}

// Apply a ReferenceChange element's ReferenceChangeP adjustments to a RefState
// (see referencechange.md): a species, an absolute or delta energy/momentum, and
// an absolute or delta reference time. Clearing has_E/has_pc lets the subsequent
// complete_energy() recompute the partner quantity with the new mass/energy.
static void apply_ref_change(const ryml::Tree& t, size_t ele, RefState& r) {
    size_t rc = t.find_child(ele, ryml::to_csubstr("ReferenceChangeP"));
    if (rc == ryml::NONE) return;

    std::string sp;
    if (get_str_child(t, rc, "species_ref", sp)) {
        r.species = sp;
        r.has_species = true;
        r.has_pc = false;  // recompute momentum for the new mass
    }
    double v;
    if (get_num_child(t, rc, "E_tot_ref", v)) {
        r.E_tot = v;
        r.has_E = true;
        r.has_pc = false;
    } else if (get_num_child(t, rc, "pc_ref", v)) {
        r.pc = v;
        r.has_pc = true;
        r.has_E = false;
    } else if (get_num_child(t, rc, "dE_ref", v)) {
        if (r.has_E) {
            r.E_tot += v;
            r.has_pc = false;
        }
    } else if (get_num_child(t, rc, "dpc_ref", v)) {
        if (r.has_pc) {
            r.pc += v;
            r.has_E = false;
        }
    }
    if (get_num_child(t, rc, "time_ref", v))
        r.time = v;
    else if (get_num_child(t, rc, "dtime_ref", v))
        r.time += v;
}

// Reference parameters at the downstream end of an element, given its completed
// upstream parameters. Species and energy carry through unchanged for most
// kinds; an RFCavity shifts the energy by RFP.dE_ref and a ReferenceChange
// applies its ReferenceChangeP. The reference time advances by the transit time
// (reference.md).
static RefState downstream_ref(const ryml::Tree& t, size_t ele,
                               const std::string& kind, const RefState& up,
                               double length, ProblemList& problems) {
    RefState down = up;

    if (kind == "RFCavity") {
        size_t rfp = t.find_child(ele, ryml::to_csubstr("RFP"));
        double dE;
        if (get_num_child(t, rfp, "dE_ref", dE) && down.has_E) {
            down.E_tot = up.E_tot + dE;
            down.has_pc = false;  // recompute momentum for the new energy
        }
    } else if (kind == "ReferenceChange") {
        apply_ref_change(t, ele, down);
    } else if (kind == "Foil") {
        // A Foil can strip electrons, changing the downstream species (and hence
        // energy). The stripping model is not defined by the standard, so the
        // downstream species is left equal to the upstream one and the omission
        // is flagged rather than guessed at.
        std::string ename = t.has_key(ele) ? std::string(t.key(ele).str,
                                                          t.key(ele).len)
                                           : "<foil>";
        add_problem(problems,
                    "Foil element '" + ename +
                        "': downstream species change is not computed",
                    ename, Severity::Warning, Origin::Unsupported);
    }

    complete_energy(down);  // refill whichever of E/pc a change above cleared

    // Reference transit time: length * (E_up + E_down) / (c * (pc_up + pc_down)).
    if (up.has_E && down.has_E && up.has_pc && down.has_pc) {
        double denom = apc::C_LIGHT * (up.pc + down.pc);
        if (denom != 0.0)
            down.time = up.time + length * (up.E_tot + down.E_tot) / denom;
    }
    return down;
}

// Read an element's FloorP group into a FloorState. Missing components default
// to zero (origin / identity orientation). Returns false if the group is absent.
static bool read_floor(const ryml::Tree& t, size_t ele, pals::FloorState& s) {
    size_t fp = t.find_child(ele, ryml::to_csubstr("FloorP"));
    if (fp == ryml::NONE) return false;
    double x = 0, y = 0, z = 0, th = 0, ph = 0, ps = 0;
    get_num_child(t, fp, "x", x);
    get_num_child(t, fp, "y", y);
    get_num_child(t, fp, "z", z);
    get_num_child(t, fp, "theta", th);
    get_num_child(t, fp, "phi", ph);
    get_num_child(t, fp, "psi", ps);
    s.r = pals::Vec3{x, y, z};
    s.q = pals::quat_from_floor_angles(pals::FloorAngles{th, ph, ps});
    return true;
}

// Write a FloorState back into an element's FloorP group (creating it),
// converting the orientation quaternion to (theta, phi, psi).
static void write_floor(ryml::Tree& t, size_t ele, const pals::FloorState& s) {
    size_t fp = find_or_add_map_child(t, ele, "FloorP");
    set_num_child(t, fp, "x", s.r.x);
    set_num_child(t, fp, "y", s.r.y);
    set_num_child(t, fp, "z", s.r.z);
    pals::FloorAngles a = pals::floor_angles_from_quat(s.q);
    set_num_child(t, fp, "theta", a.theta);
    set_num_child(t, fp, "phi", a.phi);
    set_num_child(t, fp, "psi", a.psi);
}

// Build the floor displacement L and coordinate rotation S for an element's
// reference curve, dispatched on kind: bends use their BendP angle/tilt, patches
// their PatchP offsets/rotations, everything else is a straight segment of the
// element length. (A FloorShift places its exit end outright; see
// coordinate_set_frame.)
static void element_LS(const ryml::Tree& t, size_t ele, const std::string& kind,
                       double length, pals::Vec3& L, pals::Quat& S) {
    if (kind == "Bend") {
        size_t bp = t.find_child(ele, ryml::to_csubstr("BendP"));
        double angle = 0.0;
        if (!get_num_child(t, bp, "angle_ref", angle)) {
            double g = 0.0;
            if (get_num_child(t, bp, "g_ref", g)) angle = g * length;
        }
        double tilt = 0.0;
        get_num_child(t, bp, "tilt_ref", tilt);
        pals::bend_LS(length, angle, tilt, L, S);
    } else if (kind == "Patch") {
        size_t pp = t.find_child(ele, ryml::to_csubstr("PatchP"));
        double xo = 0, yo = 0, zo = 0, xr = 0, yr = 0, zr = 0;
        get_num_child(t, pp, "x_offset", xo);
        get_num_child(t, pp, "y_offset", yo);
        get_num_child(t, pp, "z_offset", zo);
        get_num_child(t, pp, "x_rot", xr);
        get_num_child(t, pp, "y_rot", yr);
        get_num_child(t, pp, "z_rot", zr);
        pals::patch_LS(xo, yo, zo, xr, yr, zr, L, S);
    } else {
        pals::straight_LS(length, L, S);
    }
}

// The name of an element (its map key), or a placeholder for the unkeyed case.
static std::string ele_name(const ryml::Tree& t, size_t ele) {
    return t.has_key(ele) ? std::string(t.key(ele).str, t.key(ele).len)
                          : "<element>";
}

// Two numbers agree "to high accuracy" (rf.md): equal within a relative
// tolerance, with both-zero counted as equal.
static bool approx_eq(double a, double b) {
    double scale = std::max(std::fabs(a), std::fabs(b));
    return scale == 0.0 || std::fabs(a - b) <= 1e-9 * scale;
}

// ---------------------------------------------------------------------------
// DEPENDENT PARAMETERS
//
// Many parameters within a group are not independent: one is a fixed multiple
// (or the reciprocal) of another. The expanded lattice holds every non-zero
// parameter, so when the author sets one member of such a relation the parser
// derives the rest. When the author sets more than one, the parser instead
// verifies they agree to high accuracy and flags an inconsistency (rather than
// silently overwriting). link_pair / reciprocal_link are the two primitives;
// the per-group resolvers below wire them up per the parameter docs.
// ---------------------------------------------------------------------------

// Relate two proportional parameters of a group, `b_key = factor * a_key`.
//  - both present  -> verify agreement, flag an inconsistency if not.
//  - only `a`      -> derive b = factor*a (unless the result is zero: a zero
//                     parameter is not "held").
//  - only `b`      -> derive a = b/factor (needs an invertible, non-zero factor).
// Returns true if it wrote a value, so callers can iterate to a fixed point.
static bool link_pair(ryml::Tree& t, size_t group, const char* a_key,
                      const char* b_key, double factor, const std::string& ctx,
                      ProblemList& problems) {
    double a, b;
    bool has_a = get_num_child(t, group, a_key, a);
    bool has_b = get_num_child(t, group, b_key, b);
    if (has_a && has_b) {
        double expect = factor * a;
        if (!approx_eq(b, expect)) {
            std::ostringstream m;
            m << ctx << ": '" << b_key << "' and '" << a_key
              << "' are inconsistent (" << b_key << " = " << format_double(b)
              << ", but " << format_double(factor) << " * " << a_key << " = "
              << format_double(expect) << ").";
            add_problem(problems, m.str());
        }
        return false;
    }
    if (has_a && factor * a != 0.0) {
        set_num_child(t, group, b_key, factor * a);
        return true;
    }
    if (has_b && factor != 0.0 && b / factor != 0.0) {
        set_num_child(t, group, a_key, b / factor);
        return true;
    }
    return false;
}

// Relate two reciprocal parameters of a group, `b_key = 1 / a_key` (e.g.
// `radius_ref = 1 / g_ref`). A zero value has no finite reciprocal, so it is
// neither a source nor a target. Consistency (a*b == 1) is only meaningful when
// both are non-zero. Returns true if it wrote a value.
static bool reciprocal_link(ryml::Tree& t, size_t group, const char* a_key,
                            const char* b_key, const std::string& ctx,
                            ProblemList& problems) {
    double a, b;
    bool has_a = get_num_child(t, group, a_key, a);
    bool has_b = get_num_child(t, group, b_key, b);
    if (has_a && has_b) {
        if (a != 0.0 && b != 0.0 && !approx_eq(a * b, 1.0)) {
            std::ostringstream m;
            m << ctx << ": '" << a_key << "' and '" << b_key
              << "' are inconsistent (" << a_key << " * " << b_key << " = "
              << format_double(a * b) << ", expected 1).";
            add_problem(problems, m.str());
        }
        return false;
    }
    if (has_a && a != 0.0) {
        set_num_child(t, group, b_key, 1.0 / a);
        return true;
    }
    if (has_b && b != 0.0) {
        set_num_child(t, group, a_key, 1.0 / b);
        return true;
    }
    return false;
}

// Store a value the bend geometry determines, or check the author's against it.
// Same contract as link_pair / reciprocal_link: an absent parameter is filled in
// (a zero is not "held"), a present one is verified and an inconsistency
// reported rather than silently overwritten.
static void derive_or_check(ryml::Tree& t, size_t node, const char* key,
                            double value, const std::string& ctx,
                            ProblemList& problems) {
    double have;
    if (get_num_child(t, node, key, have)) {
        if (!approx_eq(have, value)) {
            std::ostringstream m;
            m << ctx << ": '" << key
              << "' is inconsistent with the rest of the bend geometry (" << key
              << " = " << format_double(have) << ", but the geometry gives "
              << format_double(value) << ").";
            add_problem(problems, m.str());
        }
        return;
    }
    if (value != 0.0) set_num_child(t, node, key, value);
}

// sin(x)/x, continued through the removable singularity at the origin. The bend
// lengths are all of this shape, and taking them this way keeps a bend that is
// nearly (or exactly) straight on the same formulas as a curved one.
static double sinc(double x) { return x == 0.0 ? 1.0 : std::sin(x) / x; }

// Tie together the three interchangeable forms of a bend's curvature:
//   radius_ref = 1 / g_ref
//   g_ref      = factor * Bn0_ref     (factor = q*c/pc)
// Any one of them fills the other two; two that disagree are flagged. The field
// leg needs the reference momentum, so it runs only when `has_factor`.
static void link_bend_curvature(ryml::Tree& t, size_t bp, bool has_factor,
                                double factor, const std::string& ctx,
                                ProblemList& problems) {
    for (int pass = 0; pass < 3; ++pass) {
        bool changed =
            reciprocal_link(t, bp, "g_ref", "radius_ref", ctx, problems);
        if (has_factor)
            changed |=
                link_pair(t, bp, "Bn0_ref", "g_ref", factor, ctx, problems);
        if (!changed) break;
    }
}

// Resolve the BendP dependent parameters (bend.md, "Dependent parameters
// calculation").
//
// The shape of a bend is fixed by any two of: its curvature (`g_ref`,
// `Bn0_ref` or `radius_ref`), one of its lengths (`length`, `L_chord` or
// `L_rectangle`), and its bend angle (`angle_ref`) -- two taken from different
// ones of those three sets, which is exactly what the standard lets an author
// give. Reduced to the arc `length` and the `angle_ref` it turns through, the
// rest are (with angle = g_ref * length):
//   L_chord     = length * sinc(angle/2)     entrance origin to exit origin
//   L_rectangle = length * sinc(angle)       that chord along the entrance axis
//   L_sagitta   = length * angle/8 * sinc(angle/4)^2
// Written this way nothing divides by the curvature, so the straight bend is not
// a special case: at zero angle the three lengths collapse to `length`, `length`
// and zero on their own.
//
// Two lengths on their own (an arc and a chord, say) are left alone: they do fix
// the shape, but only through a transcendental inversion, and the standard does
// not allow the pair anyway. A value the author set is never overwritten -- it
// is checked against what the geometry gives, and an inconsistency reported.
static void resolve_bend(ryml::Tree& t, size_t ele, double& length,
                         bool has_factor, double factor,
                         const std::string& ename, ProblemList& problems) {
    size_t bp = t.find_child(ele, ryml::to_csubstr("BendP"));
    if (bp == ryml::NONE) return;
    std::string ctx = "element '" + ename + "' BendP";
    std::string ele_ctx = "element '" + ename + "'";

    link_bend_curvature(t, bp, has_factor, factor, ctx, problems);

    // What the author gave. A zero is no constraint: a bend of zero curvature,
    // zero angle or zero length says nothing about the rest of the geometry.
    double g = 0.0, angle = 0.0, L = 0.0, chord = 0.0, rect = 0.0;
    bool has_g = get_num_child(t, bp, "g_ref", g) && g != 0.0;
    bool has_angle = get_num_child(t, bp, "angle_ref", angle) && angle != 0.0;
    bool has_L = get_num_child(t, ele, "length", L) && L != 0.0;
    bool has_chord = get_num_child(t, bp, "L_chord", chord) && chord != 0.0;
    bool has_rect = get_num_child(t, bp, "L_rectangle", rect) && rect != 0.0;

    // Reduce whichever pair was given to (length, angle_ref). The curvature and
    // the arc give the angle directly; a chord or a rectangle needs an arcsine,
    // taken on the principal branch -- the bend does not turn past a half circle
    // (a quarter, for the rectangle).
    if (!has_angle && has_g) {
        if (has_L) {
            angle = g * L;
            has_angle = true;
        } else if (has_chord || has_rect) {
            // chord = 2 sin(angle/2) / g_ref;  rectangle = sin(angle) / g_ref
            double part = has_chord ? 2.0 : 1.0;
            double len = has_chord ? chord : rect;
            const char* key = has_chord ? "L_chord" : "L_rectangle";
            double sine = g * len / part;
            if (std::fabs(sine) > 1.0) {
                add_problem(problems,
                            ctx + ": '" + key + "' (" + format_double(len) +
                                ") is too long for the curvature 'g_ref' (" +
                                format_double(g) +
                                "); no bend of that radius reaches it.");
            } else {
                angle = part * std::asin(sine);
                has_angle = angle != 0.0;
            }
        }
    }
    // The arc that goes with the angle. A bend that has a length but neither
    // curvature nor angle keeps it, and stays straight.
    if (!has_L && has_angle) {
        if (has_g)
            L = angle / g;
        else if (has_chord)
            L = chord / sinc(angle / 2.0);
        else if (has_rect)
            L = rect / sinc(angle);
        has_L = L != 0.0;
    }

    if (has_L) {
        derive_or_check(t, ele, "length", L, ele_ctx, problems);
        derive_or_check(t, bp, "angle_ref", angle, ctx, problems);
        derive_or_check(t, bp, "g_ref", angle / L, ctx, problems);
        // radius_ref and Bn0_ref follow from a g_ref the geometry just supplied.
        link_bend_curvature(t, bp, has_factor, factor, ctx, problems);
        derive_or_check(t, bp, "L_chord", L * sinc(angle / 2.0), ctx, problems);
        derive_or_check(t, bp, "L_rectangle", L * sinc(angle), ctx, problems);
        double quarter = sinc(angle / 4.0);
        derive_or_check(t, bp, "L_sagitta",
                        L * angle / 8.0 * quarter * quarter, ctx, problems);
    }

    // The element length may have just been derived; later resolvers integrate
    // multipole components over it.
    get_num_child(t, ele, "length", length);
}

// Default the actual bending field from the reference one (bend.md,
// `Kn0_from_g_ref`). The field the particle actually sees is the dipole
// component of MagneticMultipoleP, and unless the author says otherwise a bend
// bends its own reference particle along the reference curve: `Kn0` = `g_ref`,
// equivalently `Bn0` = `Bn0_ref`. `Kn0_from_g_ref: false` leaves the component
// alone, for a bend whose actual field is zero or comes from somewhere else.
//
// All four forms of the dipole component count as "set" -- `Kn0L` and `Bn0L`
// state the same field as `Kn0` and `Bn0`, only integrated over the length --
// and only the normal ones: a skew dipole is a separate component, not this one.
// The group is created when the default applies to a bend that carries none,
// since otherwise the field would have nowhere to go. Whichever form is written
// here, resolve_magnetic_multipoles fills the other three next.
static void resolve_bend_actual_field(ryml::Tree& t, size_t ele) {
    size_t bp = t.find_child(ele, ryml::to_csubstr("BendP"));
    if (bp == ryml::NONE) return;
    std::string flag = child_val_str(t, bp, "Kn0_from_g_ref");
    if (!flag.empty() && !is_true_flag(flag)) return;  // absent means true

    size_t mp = t.find_child(ele, ryml::to_csubstr("MagneticMultipoleP"));
    if (mp != ryml::NONE)
        for (const char* k : {"Kn0", "Kn0L", "Bn0", "Bn0L"})
            if (t.find_child(mp, ryml::to_csubstr(k)) != ryml::NONE) return;

    // g_ref and Bn0_ref are tied together by the curvature linking above, so
    // g_ref is the one to take whenever the reference momentum was known;
    // Bn0_ref carries the field when it was not. A zero reference bend field is
    // no field at all, and a zero is not held.
    double g = 0.0, B = 0.0;
    const char* key = nullptr;
    double value = 0.0;
    if (get_num_child(t, bp, "g_ref", g) && g != 0.0) {
        key = "Kn0";
        value = g;
    } else if (get_num_child(t, bp, "Bn0_ref", B) && B != 0.0) {
        key = "Bn0";
        value = B;
    }
    if (!key) return;

    if (mp == ryml::NONE)
        mp = find_or_add_map_child(t, ele, "MagneticMultipoleP");
    set_num_child(t, mp, key, value);
}

// Split a multipole component key into its (normal/skew char, order digits),
// e.g. "Bn2L" -> ('n', "2"). `prefixes` is the pair of accepted leading tokens
// (magnetic "Bn"/"Bs"/"Kn"/"Ks" collapse to the field forms here; electric
// "En"/"Es"). Returns false for taper keys (an underscore) and anything that is
// not <prefix><digits>[L].
static bool parse_multipole_key(const std::string& k, char first,
                                char& ns_out, std::string& order_out) {
    if (k.size() < 3 || k[0] != first) return false;
    if (k[1] != 'n' && k[1] != 's') return false;
    if (k.find('_') != std::string::npos) return false;  // *_taper etc.
    std::string rest = k.substr(2);
    if (!rest.empty() && rest.back() == 'L') rest.pop_back();
    if (rest.empty()) return false;
    for (char c : rest)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    ns_out = k[1];
    order_out = rest;
    return true;
}

// Collect the set of (normal/skew, order) components present in a multipole
// group, judged by the field-form prefix `first` ('B' magnetic, 'E' electric)
// and its normalized partner `norm` ('K', or '\0' when there is none).
static std::set<std::pair<char, std::string>> multipole_components(
    const ryml::Tree& t, size_t group, char first, char norm) {
    std::set<std::pair<char, std::string>> comps;
    for (size_t c = t.first_child(group); c != ryml::NONE;
         c = t.next_sibling(c)) {
        if (!t.has_key(c)) continue;
        std::string k(t.key(c).str, t.key(c).len);
        char ns;
        std::string order;
        if (parse_multipole_key(k, first, ns, order) ||
            (norm && parse_multipole_key(k, norm, ns, order)))
            comps.emplace(ns, order);
    }
    return comps;
}

// Resolve MagneticMultipoleP components (magneticmultipole.md). Each component
// of order N has four equivalent forms tied by length and reference momentum:
//   BnNL = length * BnN,   KnN = factor * BnN,   KnNL = factor * length * BnN
// (and likewise for the skew "s" forms). Length-integration needs no momentum;
// the field<->normalized legs run only when `has_factor`. Iterated to a fixed
// point so a value given in any one form fills the rest.
static void resolve_magnetic_multipoles(ryml::Tree& t, size_t ele, double length,
                                        bool has_factor, double factor,
                                        const std::string& ename,
                                        ProblemList& problems) {
    size_t mp = t.find_child(ele, ryml::to_csubstr("MagneticMultipoleP"));
    if (mp == ryml::NONE) return;
    std::string ctx = "element '" + ename + "' MagneticMultipoleP";

    for (const auto& comp : multipole_components(t, mp, 'B', 'K')) {
        std::string B = std::string("B") + comp.first + comp.second;
        std::string BL = B + "L";
        std::string K = std::string("K") + comp.first + comp.second;
        std::string KL = K + "L";
        for (int pass = 0; pass < 4; ++pass) {
            bool changed = false;
            changed |= link_pair(t, mp, B.c_str(), BL.c_str(), length, ctx,
                                 problems);
            changed |= link_pair(t, mp, K.c_str(), KL.c_str(), length, ctx,
                                 problems);
            if (has_factor) {
                changed |= link_pair(t, mp, B.c_str(), K.c_str(), factor, ctx,
                                     problems);
                changed |= link_pair(t, mp, BL.c_str(), KL.c_str(), factor, ctx,
                                     problems);
            }
            if (!changed) break;
        }
    }
}

// Resolve ElectricMultipoleP components (electricmultipole.md). Electric
// multipoles have no normalized form, so a component of order N is only tied by
// length-integration: EnNL = length * EnN (and the skew "s" forms). No momentum
// is needed.
static void resolve_electric_multipoles(ryml::Tree& t, size_t ele, double length,
                                        const std::string& ename,
                                        ProblemList& problems) {
    size_t ep = t.find_child(ele, ryml::to_csubstr("ElectricMultipoleP"));
    if (ep == ryml::NONE) return;
    std::string ctx = "element '" + ename + "' ElectricMultipoleP";

    for (const auto& comp : multipole_components(t, ep, 'E', '\0')) {
        std::string E = std::string("E") + comp.first + comp.second;
        std::string EL = E + "L";
        link_pair(t, ep, E.c_str(), EL.c_str(), length, ctx, problems);
    }
}

// Compute the field-dependent output parameters of an element. Uses the
// element's *upstream* reference (parameters are referenced to the upstream
// end). The reference momentum sets the field<->normalized conversion factor
// K = factor * B; when the species/momentum is unknown the momentum-dependent
// legs are skipped, but the purely geometric relations (bend angle/radius,
// multipole length-integration) still run.
//
// `length` is in/out: a bend given its angle and curvature but no length has one
// derived, and the multipole integrations that follow use it.
static void compute_dependent(ryml::Tree& t, size_t ele, const std::string& kind,
                              const RefState& up, double& length,
                              ProblemList& problems) {
    bool has_factor = false;
    double factor = 0.0;
    if (up.has_species && up.has_pc && up.pc > 0.0) {
        try {
            factor = apc::charge_of(up.species) * apc::C_LIGHT / up.pc;
            has_factor = true;
        } catch (...) {
            has_factor = false;
        }
    }

    std::string ename = ele_name(t, ele);
    if (kind == "Bend") {
        resolve_bend(t, ele, length, has_factor, factor, ename, problems);
        // Before the multipoles: this seeds the dipole component they resolve.
        resolve_bend_actual_field(t, ele);
    }
    resolve_magnetic_multipoles(t, ele, length, has_factor, factor, ename,
                                problems);
    resolve_electric_multipoles(t, ele, length, ename, problems);
}

// Resolve the RFP dependent parameters (rf.md). The active length `L_active`
// defaults to the element length `L`; it is materialized when non-zero. The RF
// voltage and gradient are then tied by `voltage = gradient * L_active`: one is
// derived from the other, or an inconsistency is flagged when both are set.
// (frequency <-> harmon is *not* resolved here: it needs the change in reference
// time across the whole branch, which is not known element-by-element.) Any
// element carrying an RFP group is handled, keyed on the group's presence.
static void compute_rf_dependent(ryml::Tree& t, size_t ele, double length,
                                 ProblemList& problems) {
    size_t rfp = t.find_child(ele, ryml::to_csubstr("RFP"));
    if (rfp == ryml::NONE) return;

    // L_active defaults to the element length; hold it when non-zero.
    double L_active = 0.0;
    if (!get_num_child(t, rfp, "L_active", L_active)) {
        L_active = length;
        if (L_active != 0.0) set_num_child(t, rfp, "L_active", L_active);
    }

    std::string ctx = "element '" + ele_name(t, ele) + "' RFP";
    link_pair(t, rfp, "gradient", "voltage", L_active, ctx, problems);
}

// Parameters with a non-zero (or enum/boolean) default, filled in for any group
// that is *present* in the element. The expanded lattice holds every non-zero
// parameter, so an author who writes a group but omits these gets the defaults
// made explicit. A group absent from the element is left untouched -- it is
// added only by the parser (ReferenceP, FloorP) or to hold a derived value with
// nowhere else to go (MagneticMultipoleP on a curved bend, see
// resolve_bend_actual_field). Parameters whose default is zero/null/false are
// not listed: they are not "held".
struct GroupDefault {
    const char* group;
    const char* key;
    const char* value;
};
static const GroupDefault kGroupDefaults[] = {
    {"RFP", "cavity_type", "STANDING_WAVE"},
    {"RFP", "zero_phase", "ACCELERATING"},
    {"BendP", "ref_geometry", "ARC"},
    {"BendP", "multipole_geometry", "FOLLOWS_REF_GEOMETRY"},
    {"BendP", "Kn0_from_g_ref", "true"},
    {"ApertureP", "shape", "ELLIPTICAL"},
    {"ApertureP", "location", "ENTRANCE_END"},
    {"ApertureP", "aperture_active", "true"},
    {"CoordinateSetP", "origin_ele", "GLOBAL_ORIGIN"},
    {"CoordinateSetP", "origin_ele_ref_pt", "EXIT_END"},
    {"ForkP", "propagate_reference", "true"},
};

// Write the non-zero/enum defaults (kGroupDefaults) into every group the element
// already carries, leaving any value the author set in place.
static void materialize_group_defaults(ryml::Tree& t, size_t ele) {
    for (const GroupDefault& d : kGroupDefaults) {
        size_t g = t.find_child(ele, ryml::to_csubstr(d.group));
        if (g == ryml::NONE || !t.is_map(g)) continue;
        if (t.find_child(g, ryml::to_csubstr(d.key)) == ryml::NONE)
            set_plain_child(t, g, d.key, d.value);
    }
}

// Where a branch's first element takes its reference and floor from when the
// branch was made by a Fork (fork.md, s:forking): the Fork's floor always, its
// reference only when `propagate_reference` is true.
struct ForkSeed {
    size_t fork = ryml::NONE;
    bool reference = false;
};

// The per-element bookkeeper. Given the previous element in the branch (NONE for
// the first, the BeginningEle) and the current element, compute and store the
// current element's upstream reference parameters, floor placement, s-position,
// and field-dependent parameters. Order matters: the reference momentum is
// needed for the dependent-parameter conversions, so reference comes first.
//
// `seed` names the Fork element that instantiated this branch, for the first
// element of a branch a Fork created. `exits` holds the exit placements of the
// elements that set theirs outright (FloorShift), which take the place of
// propagating through them.
static void _element_bookkeeper(ryml::Tree& t, size_t prev, size_t ele,
                                ProblemList& problems,
                                const std::map<size_t, pals::FloorState>& exits,
                                ForkSeed seed = ForkSeed()) {
    std::string kind = child_val_str(t, ele, "kind");

    RefState up;
    pals::FloorState floor;
    double s_pos = 0.0;

    if (prev == ryml::NONE) {
        // First element of the branch (BeginningEle): reference, floor and
        // s-position are user inputs. Complete the reference (fill pc from E or
        // vice versa) and normalize the floor placement; both default sensibly
        // when unset (zero energy/momentum, origin, identity orientation, s = 0).
        // A branch a Fork made takes the Fork's floor instead, and its reference
        // too when the Fork propagates it. A Fork has zero length and a unit
        // transfer map, so its stored (upstream) reference and floor are also
        // its values at the connection point. The s-coordinate of the new
        // branch still starts at zero.
        bool take_ref = seed.fork != ryml::NONE && seed.reference;
        up = read_ref(t, take_ref ? seed.fork : ele);
        complete_energy(up);
        read_floor(t, seed.fork != ryml::NONE ? seed.fork : ele, floor);
        if (seed.fork == ryml::NONE) get_num_child(t, ele, "s_position", s_pos);
    } else {
        // Any later element: its upstream parameters are the previous element's
        // downstream parameters, propagated through the previous element.
        std::string pkind = child_val_str(t, prev, "kind");
        double plen = 0.0;
        get_num_child(t, prev, "length", plen);

        RefState pup = read_ref(t, prev);
        complete_energy(pup);
        up = downstream_ref(t, prev, pkind, pup, plen, problems);

        auto ex = exits.find(prev);
        if (ex != exits.end()) {
            floor = ex->second;
        } else {
            pals::FloorState pfloor;
            read_floor(t, prev, pfloor);
            pals::Vec3 L;
            pals::Quat S;
            element_LS(t, prev, pkind, plen, L, S);
            floor = pals::floor_propagate(pfloor, L, S);
        }

        double ps = 0.0;
        get_num_child(t, prev, "s_position", ps);
        s_pos = ps + plen;
    }

    // Store the outputs. Reference before dependent: the latter reads pc_ref.
    write_ref(t, ele, up);
    write_floor(t, ele, floor);
    set_num_child(t, ele, "s_position", s_pos);

    // Fill in dependent parameters and non-zero/enum defaults of the groups the
    // element carries. The expanded lattice holds every non-zero parameter.
    double length = 0.0;
    get_num_child(t, ele, "length", length);
    materialize_group_defaults(t, ele);
    compute_dependent(t, ele, kind, up, length, problems);
    compute_rf_dependent(t, ele, length, problems);
}

// ---------------------------------------------------------------------------
// FLOOR PLACEMENT: FloorShift, Fiducial, Girder and ShiftedFloorP
// ---------------------------------------------------------------------------
//
// Most elements are placed by propagating along the branch. Three kinds place
// something outright, through a CoordinateSetP (coordinateset.md): a coordinate
// system taken at a reference point of an origin element (or the global origin),
// shifted by the group's offsets and rotations, and then by the element's own
// BodyShiftP.
//
//  - A FloorShift sets the placement of its exit end, and so of everything
//    downstream of it, leaving everything upstream alone.
//  - A Fiducial (zero length) fixes its own placement, and so moves the elements
//    both upstream and downstream of it: the stretch of the branch around it,
//    from the branch start or the last FloorShift to the next FloorShift, is
//    one rigid piece, and is carried bodily to where the Fiducial says.
//  - A Girder (outside the lattice) has a nominal placement from its
//    CoordinateSetP, and moves the elements it supports by its BodyShiftP.
//
// A branch is therefore placed in segments, each ending at a FloorShift or at
// the branch end. Once a segment is propagated, a Fiducial in it is applied,
// and only then is the segment final: a FloorShift exit, a later Fiducial, or a
// Girder may refer to an element only once that element's placement is final.

// Everything the floor pass needs to know across branches.
struct FloorContext {
    size_t lat_node = ryml::NONE;
    std::map<size_t, pals::FloorState> exits;  // FloorShift exit placements
    std::set<size_t> final_floor;              // elements whose placement is final
    // Fixed placements that are not a Fiducial's own: the destination of a Fork
    // into a new branch, when that is not the branch's first element.
    std::map<size_t, pals::FloorState> fixed;
};

// The rigid shift (L, S) of an offset/rotation sextet -- a BodyShiftP or the
// shift part of a CoordinateSetP -- as a placement relative to the frame it
// shifts (Eq. swww2).
static pals::FloorState shift_of(const ryml::Tree& t, size_t group) {
    double xo = 0, yo = 0, zo = 0, xr = 0, yr = 0, zr = 0;
    get_num_child(t, group, "x_offset", xo);
    get_num_child(t, group, "y_offset", yo);
    get_num_child(t, group, "z_offset", zo);
    get_num_child(t, group, "x_rot", xr);
    get_num_child(t, group, "y_rot", yr);
    get_num_child(t, group, "z_rot", zr);
    pals::FloorState sh;
    pals::patch_LS(xo, yo, zo, xr, yr, zr, sh.r, sh.q);
    return sh;
}

// The placement of an element's exit end.
static pals::FloorState exit_floor(const ryml::Tree& t, size_t ele,
                                   const FloorContext& ctx) {
    auto ex = ctx.exits.find(ele);
    if (ex != ctx.exits.end()) return ex->second;
    pals::FloorState f;
    read_floor(t, ele, f);
    double len = 0.0;
    get_num_child(t, ele, "length", len);
    pals::Vec3 L;
    pals::Quat S;
    element_LS(t, ele, child_val_str(t, ele, "kind"), len, L, S);
    return pals::floor_propagate(f, L, S);
}

// The frame, relative to an element's entrance placement, about which its
// BodyShiftP is measured: the middle of a straight element, the middle of the
// chord of a bend (coordinates.md, s:straight.mis and s:bend.mis). A Patch has
// no body to shift and is measured from its entrance.
static pals::FloorState body_reference(const ryml::Tree& t, size_t ele) {
    std::string kind = child_val_str(t, ele, "kind");
    double len = 0.0;
    get_num_child(t, ele, "length", len);
    pals::FloorState ref;
    if (kind == "Patch") return ref;
    if (kind == "Bend") {
        size_t bp = t.find_child(ele, ryml::to_csubstr("BendP"));
        double angle = 0.0;
        if (!get_num_child(t, bp, "angle_ref", angle)) {
            double g = 0.0;
            if (get_num_child(t, bp, "g_ref", g)) angle = g * len;
        }
        double tilt = 0.0;
        get_num_child(t, bp, "tilt_ref", tilt);
        pals::bend_LS(len / 2, angle / 2, tilt, ref.r, ref.q);
        if (std::fabs(angle) > 1e-12) {
            // Mid-arc to mid-chord, toward the centre of curvature.
            double rho = len / angle;
            double d = rho * (std::cos(angle / 2) - 1.0);
            pals::FloorState chord;
            chord.r = pals::Vec3{d * std::cos(tilt), d * std::sin(tilt), 0.0};
            ref = pals::floor_compose(ref, chord);
        }
        return ref;
    }
    pals::straight_LS(len / 2, ref.r, ref.q);
    return ref;
}

// The placement of the coordinate system an element's CoordinateSetP describes,
// shifted further by the element's BodyShiftP when `with_body` is set. `ele` is
// the element holding the group; `prev_exit` the exit of the element before it
// in its branch (null when there is none, or none whose placement is final
// yet). Returns false, with a problem recorded, when the origin cannot be
// placed.
static bool coordinate_set_frame(const ryml::Tree& t, size_t ele,
                                 const pals::FloorState* prev_exit,
                                 const FloorContext& ctx, bool with_body,
                                 pals::FloorState& out, ProblemList& problems) {
    std::string name = ele_name(t, ele);
    size_t cs = t.find_child(ele, ryml::to_csubstr("CoordinateSetP"));
    std::string origin = "GLOBAL_ORIGIN";
    std::string ref_pt = "EXIT_END";
    if (cs != ryml::NONE) {
        get_str_child(t, cs, "origin_ele", origin);
        get_str_child(t, cs, "origin_ele_ref_pt", ref_pt);
    }

    pals::FloorState base;  // the global origin
    if (origin == "PREVIOUS_ELEMENT") {
        if (!prev_exit) {
            add_problem(problems,
                        "element '" + name +
                            "': CoordinateSetP.origin_ele is PREVIOUS_ELEMENT, "
                            "but there is no previous element whose placement "
                            "is known first",
                        name);
            return false;
        }
        base = *prev_exit;
    } else if (origin != "GLOBAL_ORIGIN") {
        ElementMatches m = match_element_parameters(t, ctx.lat_node, origin);
        if (!m.valid || m.has_param || m.elements.size() != 1) {
            add_problem(problems,
                        "element '" + name + "': CoordinateSetP.origin_ele '" +
                            origin + "' must name exactly one lattice element (" +
                            std::to_string(m.elements.size()) + " matched)",
                        name);
            return false;
        }
        size_t o = m.elements[0];
        if (!ctx.final_floor.count(o)) {
            add_problem(problems,
                        "element '" + name + "': the placement of "
                        "CoordinateSetP.origin_ele '" + origin +
                            "' is not known before that of '" + name + "'",
                        name);
            return false;
        }
        if (ref_pt == "ENTRANCE_END") {
            read_floor(t, o, base);
        } else if (ref_pt == "CENTER") {
            pals::FloorState in;
            read_floor(t, o, in);
            std::string okind = child_val_str(t, o, "kind");
            double len = 0.0;
            get_num_child(t, o, "length", len);
            pals::FloorState half;
            if (okind == "Bend") {
                size_t bp = t.find_child(o, ryml::to_csubstr("BendP"));
                double angle = 0.0, tilt = 0.0;
                if (!get_num_child(t, bp, "angle_ref", angle)) {
                    double g = 0.0;
                    if (get_num_child(t, bp, "g_ref", g)) angle = g * len;
                }
                get_num_child(t, bp, "tilt_ref", tilt);
                pals::bend_LS(len / 2, angle / 2, tilt, half.r, half.q);
            } else {
                pals::straight_LS(len / 2, half.r, half.q);
            }
            base = pals::floor_compose(in, half);
        } else {
            base = exit_floor(t, o, ctx);
        }
    }

    out = cs != ryml::NONE ? pals::floor_compose(base, shift_of(t, cs)) : base;
    size_t bs = t.find_child(ele, ryml::to_csubstr("BodyShiftP"));
    if (with_body && bs != ryml::NONE)
        out = pals::floor_compose(out, shift_of(t, bs));
    return true;
}

// Finish one segment of a branch: carry it bodily to where a Fiducial in it (or
// a fixed fork destination) puts it, then mark every element of it final.
// `prev_exit` is the exit of the element just before the segment, when that is
// final (the end of the previous segment), for a Fiducial placed relative to it.
static void finish_floor_segment(ryml::Tree& t,
                                 const std::vector<size_t>& seg,
                                 const pals::FloorState* prev_exit,
                                 FloorContext& ctx, ProblemList& problems) {
    size_t anchor = ryml::NONE;
    pals::FloorState fixed;
    for (size_t i = 0; i < seg.size(); ++i) {
        size_t e = seg[i];
        bool fiducial = child_val_str(t, e, "kind") == "Fiducial";
        auto fx = ctx.fixed.find(e);
        if (!fiducial && fx == ctx.fixed.end()) continue;
        if (anchor != ryml::NONE) {
            add_problem(problems,
                        "elements '" + ele_name(t, anchor) + "' and '" +
                            ele_name(t, e) +
                            "' both fix the placement of the same rigid "
                            "stretch of their branch; only the first is used",
                        ele_name(t, e));
            continue;
        }
        pals::FloorState where;
        if (fiducial) {
            // PREVIOUS_ELEMENT is usable only when the element before the
            // Fiducial is outside this segment, and so already final.
            const pals::FloorState* pe = i == 0 ? prev_exit : nullptr;
            if (!coordinate_set_frame(t, e, pe, ctx, true, where, problems))
                continue;
        } else {
            where = fx->second;
        }
        anchor = e;
        fixed = where;
    }

    if (anchor != ryml::NONE) {
        pals::FloorState now;
        read_floor(t, anchor, now);
        pals::FloorState motion =
            pals::floor_compose(fixed, pals::floor_inverse(now));
        for (size_t e : seg) {
            pals::FloorState f;
            read_floor(t, e, f);
            write_floor(t, e, pals::floor_compose(motion, f));
        }
    }
    for (size_t e : seg) ctx.final_floor.insert(e);
}

// Write a placement into an element's ShiftedFloorP group (creating it).
static void write_shifted_floor(ryml::Tree& t, size_t ele,
                                const pals::FloorState& s) {
    size_t fp = find_or_add_map_child(t, ele, "ShiftedFloorP");
    set_num_child(t, fp, "x", s.r.x);
    set_num_child(t, fp, "y", s.r.y);
    set_num_child(t, fp, "z", s.r.z);
    pals::FloorAngles a = pals::floor_angles_from_quat(s.q);
    set_num_child(t, fp, "theta", a.theta);
    set_num_child(t, fp, "phi", a.phi);
    set_num_child(t, fp, "psi", a.psi);
}

// The `girders` dict of the PALS node: girder name -> definition.
static std::vector<size_t> girder_defs(const ryml::Tree& t) {
    std::vector<size_t> out;
    size_t pals = t.find_child(t.root_id(), ryml::to_csubstr("PALS"));
    if (pals == ryml::NONE) return out;
    size_t g = t.find_child(pals, ryml::to_csubstr("girders"));
    if (g == ryml::NONE || !t.is_map(g)) return out;
    for (size_t c = t.first_child(g); c != ryml::NONE; c = t.next_sibling(c))
        if (t.is_map(c) && t.has_key(c) &&
            child_val_str(t, c, "kind") == "Girder")
            out.push_back(c);
    return out;
}

// Place the girders and fill in every element's ShiftedFloorP
// (shiftedfloor.md): its placement with its BodyShiftP and the shifts of the
// girders that support it (coordinates.md, s:girder.mis).
//
// A girder's nominal placement comes from its CoordinateSetP and is its FloorP;
// its BodyShiftP then moves it, and with it everything it supports, as one
// rigid body. Written as a rigid motion of the floor, the girder G moves things
// by M_G = F_G0 * Shift_G * F_G0^-1, after the motion of the girder supporting
// it, if any. An element E with reference frame F_E0 and its own shift Shift_E
// ends up at M_G * F_E0 * Shift_E, which is Eqs. lwwl-vewe.
static void place_girders_and_shifts(ryml::Tree& t, FloorContext& ctx,
                                     ProblemList& problems) {
    std::vector<size_t> girders = girder_defs(t);

    // What each girder supports, and so which girder supports what.
    std::map<size_t, size_t> support;  // element or girder -> its girder
    for (size_t g : girders) {
        std::string gname = ele_name(t, g);
        size_t gp = t.find_child(g, ryml::to_csubstr("GirderP"));
        std::string spec;
        if (!get_str_child(t, gp, "elements", spec)) {
            add_problem(problems,
                        "Girder '" + gname + "': GirderP.elements names nothing "
                        "to support",
                        gname);
            continue;
        }
        std::vector<size_t> others;
        for (size_t o : girders)
            if (o != g) others.push_back(o);
        ElementMatches m =
            match_element_parameters(t, ctx.lat_node, spec, others);
        if (!m.valid || m.has_param) {
            add_problem(problems,
                        "Girder '" + gname + "': GirderP.elements '" + spec +
                            "' is not an element set",
                        gname);
            continue;
        }
        if (m.elements.empty())
            add_problem(problems,
                        "Girder '" + gname + "': GirderP.elements '" + spec +
                            "' matches nothing in the expanded lattice",
                        gname, Severity::Warning);
        for (size_t e : m.elements) {
            auto it = support.find(e);
            if (it != support.end() && it->second != g) {
                add_problem(problems,
                            "'" + ele_name(t, e) + "' is supported by both "
                            "Girder '" + ele_name(t, it->second) +
                                "' and Girder '" + gname + "'",
                            ele_name(t, e));
                continue;
            }
            support[e] = g;
        }
    }

    // Each girder's nominal placement and motion, parents first.
    std::map<size_t, pals::FloorState> nominal, motion;
    std::set<size_t> failed;
    std::function<bool(size_t, std::set<size_t>&)> place =
        [&](size_t g, std::set<size_t>& active) -> bool {
        if (motion.count(g)) return true;
        if (failed.count(g)) return false;
        std::string gname = ele_name(t, g);
        size_t cs = t.find_child(g, ryml::to_csubstr("CoordinateSetP"));
        std::string origin;
        if (cs != ryml::NONE && get_str_child(t, cs, "origin_ele", origin) &&
            origin == "PREVIOUS_ELEMENT") {
            add_problem(problems,
                        "Girder '" + gname + "': CoordinateSetP.origin_ele may "
                        "not be PREVIOUS_ELEMENT -- a Girder sits in no branch",
                        gname);
            failed.insert(g);
            return false;
        }
        pals::FloorState f0;
        if (!coordinate_set_frame(t, g, nullptr, ctx, false, f0, problems)) {
            failed.insert(g);
            return false;
        }
        pals::FloorState parent;  // identity: no supporting girder
        auto sp = support.find(g);
        if (sp != support.end()) {
            if (active.count(sp->second)) {
                add_problem(problems,
                            "Girder '" + gname + "' is part of a circular chain "
                            "of girder support",
                            gname);
                failed.insert(g);
                return false;
            }
            active.insert(g);
            bool ok = place(sp->second, active);
            active.erase(g);
            if (!ok) {
                failed.insert(g);
                return false;
            }
            parent = motion[sp->second];
        }
        pals::FloorState shift;
        size_t bs = t.find_child(g, ryml::to_csubstr("BodyShiftP"));
        if (bs != ryml::NONE) shift = shift_of(t, bs);
        nominal[g] = f0;
        motion[g] = pals::floor_compose(
            parent, pals::floor_compose(
                        f0, pals::floor_compose(shift, pals::floor_inverse(f0))));
        return true;
    };
    for (size_t g : girders) {
        std::set<size_t> active{g};
        if (!place(g, active)) continue;
        write_floor(t, g, nominal[g]);
        write_shifted_floor(t, g, pals::floor_compose(motion[g], nominal[g]));
    }

    // ShiftedFloorP of every element of every branch.
    size_t branches = t.find_child(ctx.lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            size_t ele = t.is_map(le) ? t.first_child(le) : ryml::NONE;
            if (ele == ryml::NONE || !t.has_key(ele) || !t.is_map(ele)) continue;
            std::string kind = child_val_str(t, ele, "kind");
            if (kind == "Placeholder") continue;
            pals::FloorState in;
            if (!read_floor(t, ele, in)) continue;
            // The BodyShiftP of a FloorShift or Fiducial places the element
            // rather than misaligning it, so its two placements are the same.
            if (kind == "FloorShift" || kind == "Fiducial") {
                write_shifted_floor(t, ele, in);
                continue;
            }
            pals::FloorState ref = body_reference(t, ele);
            pals::FloorState f0 = pals::floor_compose(in, ref);
            pals::FloorState body = f0;
            size_t bs = t.find_child(ele, ryml::to_csubstr("BodyShiftP"));
            if (bs != ryml::NONE)
                body = pals::floor_compose(body, shift_of(t, bs));
            auto sp = support.find(ele);
            if (sp != support.end() && motion.count(sp->second))
                body = pals::floor_compose(motion[sp->second], body);
            write_shifted_floor(
                t, ele, pals::floor_compose(body, pals::floor_inverse(ref)));
        }
    }
}

// Stamp `element_index: n` on an element definition: the element's position in
// the branch line that holds it, counting from one.
//
// Every entry of the line is counted, so the index is the array index the
// element sits at even when an entry before it is a bare name the expansion
// could not resolve and so has no definition to fill in. The `branch_end` cap
// is an element of the line like any other and is numbered with the rest.
static void set_element_index(ryml::Tree& t, size_t ele, size_t index) {
    set_plain_child(t, ele, "element_index", std::to_string(index).c_str());
}

// Append a zero-length `Placeholder` element named `branch_end` to a branch's
// `line` and return its definition node. This is the marker that holds the
// branch's final floor placement and reference parameters; the bookkeeper fills
// those in by treating it like any other element (its upstream parameters are
// the downstream end of the last real element).
static size_t append_branch_end(ryml::Tree& t, size_t line) {
    ensure_capacity(t, 4);
    size_t wrapper = t.append_child(line);  // the anonymous seq-entry map
    t.ref(wrapper) |= ryml::MAP;
    size_t def = t.append_child(wrapper);  // the keyed element definition
    t.ref(def) |= ryml::KEY | ryml::MAP;
    t.set_key(def, t.to_arena(ryml::to_csubstr("branch_end")));
    size_t kind = t.append_child(def);
    t.ref(kind) |= ryml::KEY | ryml::VAL;
    t.set_key(kind, t.to_arena(ryml::to_csubstr("kind")));
    t.set_val(kind, t.to_arena(ryml::to_csubstr("Placeholder")));
    return def;
}

// Report a branch whose reference parameters cannot be computed.
//
// Every element takes its reference from the one before it, so the whole branch
// rests on the first element's: it is either written on that element (a
// BeginningEle's ReferenceP) or propagated in from the Fork that created the
// branch. When neither happens there is no species and no energy to propagate,
// and nothing later in the branch can supply them -- a `ReferenceP` further
// down is read as that element's own upstream values, not as a starting point
// for the ones before it. The species and the energy are reported separately
// because either alone is enough to leave the reference incomplete: without the
// species there is no mass to convert energy and momentum through, and without
// either energy or momentum there is nothing to convert.
//
// Runs after the first element has been bookkept, so `ele` holds the reference
// it ended up with, seed and all.
static void check_branch_reference(const ryml::Tree& t, size_t branch,
                                   size_t ele, ProblemList& problems) {
    RefState r = read_ref(t, ele);
    if (r.has_species && (r.has_E || r.has_pc)) return;

    const char* missing =
        !r.has_species
            ? (r.has_E || r.has_pc ? "no reference species"
                                   : "no reference species or energy")
            : "no reference energy or momentum";
    std::string branch_name =
        t.has_key(branch)
            ? std::string(t.key(branch).str, t.key(branch).len)
            : "<branch>";
    add_problem(problems,
                "branch '" + branch_name + "': first element '" +
                    ele_name(t, ele) + "' has " + missing +
                    ", and none was propagated into the branch; the reference "
                    "parameters cannot be computed");
}

// True if `ele` is the first element of the line of one of the branches.
static bool is_first_of_line(const ryml::Tree& t, size_t branches, size_t ele) {
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        size_t branch = t.is_map(entry) ? t.first_child(entry) : ryml::NONE;
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            if (!t.is_map(le)) continue;
            size_t def = t.first_child(le);
            if (def == ryml::NONE || !t.has_key(def)) continue;
            if (def == ele) return true;
            break;
        }
    }
    return false;
}

// Walk every branch of the expanded lattice and run _element_bookkeeper on each
// element in order, threading each element's results (persisted in the tree)
// into the next. After the branch's elements, a `branch_end` Placeholder is
// appended and bookkept to hold the branch's final floor placement and reference
// parameters (the downstream end of the last element). The floor placement is
// settled segment by segment as the walk goes (see FLOOR PLACEMENT), and once
// every branch is placed the girders are, and every element's ShiftedFloorP.
//
// A Fork into a new branch seeds that branch's beginning element from its own
// floor, and from its reference when its `propagate_reference` is true; when
// the destination is further into the branch, its placement is fixed to the
// Fork's instead. Branches are visited in order and a new branch is always
// appended after the Fork that creates it, so by the time a forked branch is
// reached its Fork has been bookkept and placed: `fork_seed` maps the
// destination element (by node id, matching the raw `destination_pointer`) to
// that Fork. `destination_pointer` still holds the work-tree node id here; it is
// remapped to the split-out tree only later.
static void run_element_bookkeeper(ryml::Tree& t, size_t lat_node,
                                   ProblemList& problems) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;

    FloorContext ctx;
    ctx.lat_node = lat_node;
    std::map<size_t, ForkSeed> fork_seed;  // destination element -> its Fork
    std::vector<size_t> forks;             // in walk order

    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;

        // A Fork's placement is final once its segment is, so the fixed
        // placements of destinations deeper into new branches are recorded only
        // then; they are all in place before the branches they fix are walked.
        for (size_t f : forks) {
            size_t forkp = t.find_child(f, ryml::to_csubstr("ForkP"));
            std::string ptr = child_val_str(t, forkp, "destination_pointer");
            std::string nb = child_val_str(t, forkp, "new_branch");
            if (ptr.empty() || nb.empty() || nb == "null") continue;
            size_t dest;
            try {
                dest = static_cast<size_t>(std::stoull(ptr));
            } catch (...) {
                continue;
            }
            pals::FloorState ff;
            if (!fork_seed.count(dest) && read_floor(t, f, ff))
                ctx.fixed[dest] = ff;
        }
        forks.clear();

        size_t prev = ryml::NONE;
        size_t index = 0;  // position in `line` of the entry being visited
        std::vector<size_t> seg;
        bool have_seg_entry = false;
        pals::FloorState seg_entry;  // exit of the element before the segment
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            ++index;
            // A line entry is a wrapper map whose first child is the keyed
            // element definition; bare unresolved references have no parameters.
            if (!t.is_map(le)) continue;
            size_t def = t.first_child(le);
            if (def == ryml::NONE || !t.has_key(def)) continue;

            bool first = (prev == ryml::NONE);
            ForkSeed seed;
            if (first) {
                auto it = fork_seed.find(def);
                if (it != fork_seed.end()) seed = it->second;
            }
            _element_bookkeeper(t, prev, def, problems, ctx.exits, seed);
            set_element_index(t, def, index);
            if (first) check_branch_reference(t, branch, def, problems);
            seg.push_back(def);

            std::string kind = child_val_str(t, def, "kind");
            if (kind == "FloorShift") {
                // The segment ends here: settle it, then place the exit, which
                // may lean on anything already final -- this segment included.
                finish_floor_segment(t, seg, have_seg_entry ? &seg_entry : nullptr,
                                     ctx, problems);
                seg.clear();
                pals::FloorState in, out;
                read_floor(t, def, in);
                if (!coordinate_set_frame(t, def, &in, ctx, true, out, problems))
                    out = in;
                ctx.exits[def] = out;
                seg_entry = out;
                have_seg_entry = true;
            }

            // Record where a Fork leads. Seeding applies only when the
            // destination is the beginning element of a *new* branch (fork.md):
            // a fork into an existing branch, which handle_fork resolves to
            // `new_branch: null`, never seeds.
            if (kind == "Fork") {
                size_t forkp = t.find_child(def, ryml::to_csubstr("ForkP"));
                std::string prop =
                    forkp != ryml::NONE
                        ? child_val_str(t, forkp, "propagate_reference")
                        : "";
                std::string ptr =
                    forkp != ryml::NONE
                        ? child_val_str(t, forkp, "destination_pointer")
                        : "";
                std::string nb =
                    forkp != ryml::NONE
                        ? child_val_str(t, forkp, "new_branch")
                        : "";
                bool new_branch = !nb.empty() && nb != "null";
                if (!ptr.empty() && new_branch) {
                    try {
                        size_t dest = static_cast<size_t>(std::stoull(ptr));
                        if (is_first_of_line(t, branches, dest))
                            fork_seed[dest] = ForkSeed{def, prop != "false"};
                        forks.push_back(def);
                    } catch (...) {
                    }
                }
            }
            prev = def;
        }

        // An empty branch has no final state to record; otherwise cap it with a
        // `branch_end` Placeholder carrying the downstream end of the last
        // element. A second pass (after post-expansion sets) walks a branch that
        // already carries the one the first pass appended: the loop above has
        // just bookkept it like any other element, so there is nothing to add.
        bool capped = prev != ryml::NONE &&
                      t.key(prev) == ryml::to_csubstr("branch_end");
        if (prev != ryml::NONE && !capped) {
            size_t cap = append_branch_end(t, line);
            _element_bookkeeper(t, prev, cap, problems, ctx.exits);
            // Appended past the last entry the loop counted, so it takes the
            // next index.
            set_element_index(t, cap, index + 1);
            seg.push_back(cap);
        }
        finish_floor_segment(t, seg, have_seg_entry ? &seg_entry : nullptr, ctx,
                             problems);
    }

    place_girders_and_shifts(t, ctx, problems);
}

// Map every element definition in the expanded lattice to the
// `{branch-name}>>{element-name}` form the fork parameters name elements by
// (fork.md, forkfrom.md). Built as a whole before the forks are walked because a
// Fork names a destination in a branch other than its own, which the walk has
// either already passed or not yet reached.
static std::map<size_t, std::string> qualified_element_names(ryml::Tree& t,
                                                             size_t branches) {
    std::map<size_t, std::string> names;
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.has_key(branch)) continue;
        std::string branch_name(t.key(branch).str, t.key(branch).len);
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            if (!t.is_map(le)) continue;
            size_t ele = t.first_child(le);
            if (ele == ryml::NONE || !t.has_key(ele)) continue;
            names[ele] = branch_name + ">>" +
                         std::string(t.key(ele).str, t.key(ele).len);
        }
    }
    return names;
}

/**
 * Write the two names that record, in the expanded lattice, what each `Fork`
 * connected to: `ForkP.forked_to` on the Fork, and `ForkFromP` on its
 * destination.
 *
 * `forked_to` (fork.md, s:fork.params) is an output parameter naming the
 * destination element as `{branch-name}>>{element-name}` -- the resolved form of
 * the `to_line`/`destination_element` the input asked for, so whatever the input
 * carried under that key is replaced by what the Fork actually reached.
 *
 * `ForkFromP` (forkfrom.md, s:fork.from.params) points the other way: a sequence
 * on the destination carrying one entry per incoming Fork, keyed
 * `{branch-name}>>{element-name}` after the Fork and its branch, with the
 * Fork's index in that branch's line as the value:
 *
 *     ForkFromP:
 *       - inject_line>>inj_fork: 134
 *       - alt_line>>end_fork: 37
 *
 * The index is 1-based -- entry 37 is the 37th element of `alt_line`. Both names
 * are built by the parser and exist only in the expanded lattice, so like
 * `destination_pointer` their nodes carry no provenance.
 *
 * Runs after expansion, when every branch line is final and the indices are the
 * ones the expanded lattice actually carries. The `destination_pointer`
 * scalars still hold work tree ids at this point -- remap_destination_pointers
 * renumbers them only once the lattice has been cut out into a tree of its own
 * -- so each one names its destination element directly.
 */
static void link_fork_connections(ryml::Tree& t, size_t lat_node) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;

    std::map<size_t, std::string> qnames = qualified_element_names(t, branches);

    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.has_key(branch)) continue;
        std::string branch_name(t.key(branch).str, t.key(branch).len);
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;

        int index = 0;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            // Every entry counts towards the index, including a bare name that
            // stayed unresolved -- the index is a position in the line, not a
            // count of the elements that expanded.
            index++;
            if (!t.is_map(le)) continue;
            size_t ele = t.first_child(le);
            if (ele == ryml::NONE || !t.has_key(ele)) continue;
            if (child_val_str(t, ele, "kind") != "Fork") continue;

            // A Fork that failed to resolve has no ForkP.destination_pointer,
            // and the reason was reported when it was handled; there is nothing
            // to link to.
            size_t forkp = t.find_child(ele, ryml::to_csubstr("ForkP"));
            if (forkp == ryml::NONE) continue;
            std::string ptr = child_val_str(t, forkp, "destination_pointer");
            if (ptr.empty()) continue;
            size_t dest;
            try {
                dest = static_cast<size_t>(std::stoull(ptr));
            } catch (...) {
                continue;
            }
            if (dest >= t.capacity() || !t.is_map(dest)) continue;

            // Name the destination on the Fork. A destination that is not in
            // `qnames` is not an element of any branch line, which handle_fork
            // does not produce a pointer to.
            auto dest_name = qnames.find(dest);
            if (dest_name != qnames.end())
                set_plain_child(t, forkp, "forked_to",
                                dest_name->second.c_str());

            std::string name = branch_name + ">>" +
                               std::string(t.key(ele).str, t.key(ele).len);
            size_t group = find_or_add_seq_child(t, dest, "ForkFromP");
            ensure_capacity(t, 2);
            size_t item = t.append_child(group);
            t.ref(item) |= ryml::MAP;
            size_t kv = t.append_child(item);
            t.ref(kv) |= ryml::KEY | ryml::VAL;
            t.set_key(kv, t.to_arena(ryml::to_csubstr(name)));
            t.set_val(kv,
                      t.to_arena(ryml::to_csubstr(std::to_string(index))));
        }
    }
}

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

// Visit every element definition of every branch line, in the order the paths
// number them. `fn(branch_index, line_index, def, entry)` receives the element
// definition and the anonymous seq-entry map wrapping it. The callback must not
// add or remove nodes: the walk holds sibling ids across the call.
template <typename F>
static void for_each_line_element(ryml::Tree& t, size_t lat_node, F fn) {
    size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
    if (branches == ryml::NONE || !t.is_seq(branches)) return;

    size_t b = 0;
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry), ++b) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        size_t line = t.find_child(branch, ryml::to_csubstr("line"));
        if (line == ryml::NONE || !t.is_seq(line)) continue;

        size_t l = 0;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le), ++l) {
            // A line entry is a wrapper map whose first child is the keyed
            // element definition; bare unresolved references have no parameters.
            if (!t.is_map(le)) continue;
            size_t def = t.first_child(le);
            if (def == ryml::NONE || !t.has_key(def)) continue;
            fn(b, l, def, le);
        }
    }
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
                const std::string key(t.key(c).str, t.key(c).len);
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
                                          std::string(t.key(g).str,
                                                      t.key(g).len));
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
                                  std::map<size_t, size_t>& prov) {
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
                const std::string key(t.key(c).str, t.key(c).len);
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
                            std::string(t.key(g).str, t.key(g).len)))
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
        erase_prov_subtree(t, n, prov);
        t.remove(n);
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
                                const std::map<size_t, size_t>& from_work,
                                ProblemList& problems) {
    if (node == ryml::NONE) return;

    if (t.has_key(node) &&
        t.key(node) == ryml::to_csubstr("destination_pointer") &&
        t.has_val(node)) {
        std::string old(t.val(node).str, t.val(node).len);
        size_t target = 0;
        try {
            target = static_cast<size_t>(std::stoull(old));
        } catch (...) {
            return;
        }
        auto it = from_work.find(target);
        if (it == from_work.end()) {
            add_problem(
                problems,
                "destination_pointer target is outside the expanded lattice");
            return;
        }
        std::string id_str = std::to_string(it->second);
        t.set_val(node, t.to_arena(ryml::to_csubstr(id_str)));
    }

    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        remap_destination_pointers(t, c, from_work, problems);
}

// Copy the lattice out of the work tree into a tree of its own: a map holding
// the single `name: {...}` entry. The root is synthesised (a ryml root cannot
// itself carry a key), so it has no counterpart in combined and no provenance
// entry. When there is no lattice the tree stays an empty map.
static ParsedData* split_out_lattice(ryml::Tree& t, size_t lat_node,
                                     const std::map<size_t, size_t>& work_prov,
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
    std::map<size_t, size_t> from_work;
    for (const auto& kv : out->provenance) from_work[kv.second] = kv.first;
    remap_destination_pointers(out->tree, out->tree.root_id(), from_work,
                               problems);

    chain_prov(out->provenance, work_prov);
    return out;
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

    // The `facility` `set` commands run first, on the element definitions, so
    // expansion copies the values they write into every use of a definition.
    check_definitions(t, problems);
    run_pre_expansion_sets(t, pals_list_entries(t, "facility"), work.provenance,
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
        // Before expand, so the root line a branch names only by its own key is
        // in place as an `inherit` for expansion to follow.
        default_branch_inherit(t, lat_node, emap);

        size_t branches = t.find_child(lat_node, ryml::to_csubstr("branches"));
        std::map<size_t, int> mp_pass;  // multipass line def -> traversals so far
        std::set<size_t> done;  // branches a Fork already expanded
        expand(t, lat_node, emap, work.provenance, problems, branches, mp_pass,
               done);

        // Runs after expand, not inside it: a Fork appends branches as it goes,
        // so only once expand has returned does `branches` hold them all.
        strip_branch_kinds(t, lat_node, work.provenance);
        remove_placeholders(t, lat_node, work.provenance);

        // Likewise after expand, so every branch -- root and forked alike -- has
        // had its chance to fill in, and an empty one really is empty.
        check_branches_expanded(t, lat_node, emap, problems);

        // A branch whose root line is itself `multipass` numbers its elements
        // here — the branch line is not reached through a sub-line flatten, so
        // its multipass indexing cannot happen during expand().
        number_multipass_branches(t, lat_node, work.provenance);

        // Evaluate every mathematical expression to a number (immediate and
        // expr()-delayed alike), and apply the ABSOLUTE controllers to the
        // parameters they drive -- both before the bookkeeper, so the reference
        // and dependent parameters below are computed from the driven values.
        // Node ids of existing nodes are unchanged -- only scalar text is
        // rewritten -- so provenance stays valid; a parameter a controller
        // creates is new and simply has no provenance, like ReferenceP/FloorP.
        evaluate_expressions(t, lat_node, work.provenance, problems);

        // The last point at which the lattice holds the author's inputs and
        // nothing else. Record their paths now; everything the finished lattice
        // carries that is not among them was computed, and is what tells
        // `expanded` apart from `full_expanded`.
        collect_authored(t, lat_node, authored);

        // With every input now a plain number, walk each branch element-by-
        // element to fill in the reference, floor, s-position, and field-
        // dependent output parameters. This adds new ReferenceP/FloorP nodes,
        // which carry no provenance (like destination_pointer) and so simply do not
        // appear in the correspondence map.
        run_element_bookkeeper(t, lat_node, problems);

        // `post_expansion` acts on the lattice just built: its sets reach each
        // expanded copy of an element separately, and the controllers are then
        // applied again (over both lists) so they stay authoritative over any
        // parameter a set touched. The bookkeeper runs a second time to
        // recompute what those writes invalidated -- reference and floor
        // parameters, and the family members execute_set dropped.
        if (!post.empty()) {
            std::vector<SetTarget> set_writes;
            run_post_expansion_sets(t, lat_node, post, work.provenance,
                                    problems, &set_writes);
            // These writes are inputs like any other, but they land after the
            // snapshot above; join them to it before the second bookkeeper pass
            // derives their families again.
            add_set_targets(t, lat_node, set_writes, authored);
            evaluate_expressions(t, lat_node, work.provenance, problems);
            run_element_bookkeeper(t, lat_node, problems);
        }

        // Last, so the line indices it records are the finished ones -- the
        // bookkeeper caps every branch with a `branch_end` element.
        link_fork_connections(t, lat_node);

        size_t wrapper = t.parent(lat_node);
        skip = (wrapper != ryml::NONE && !t.is_root(wrapper) &&
                t.num_children(wrapper) == 1)
                   ? wrapper
                   : lat_node;
    }

    // full_expanded: the lattice as the bookkeeper left it, everything computed.
    ParsedData* full = split_out_lattice(t, lat_node, work.provenance, problems);

    // expanded: the same lattice, pruned back to what the author wrote. It is
    // cut from the same work tree rather than from `full`, so the two are
    // independent -- freeing one leaves the other whole. Its faults are `full`'s
    // faults and have just been reported, so the second split's problems are
    // dropped rather than said twice.
    ProblemList said_already;
    ParsedData* mini =
        split_out_lattice(t, lat_node, work.provenance, said_already);
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

/**
 * Recursive helper for make_original. Walks the contents of one already-parsed
 * file and, for every file it references -- by `include` or by `load` -- adds
 * that file to `master` under its resolved path, then walks it in turn.
 *
 * `src_path` is the resolved path of the file `src` holds, which is what its
 * references are relative to.
 */
static void add_to_master_tree(ryml::Tree& master, const ryml::Tree& src,
                               size_t node, const std::string& src_path);

/**
 * Parse the file `ref` names as seen from `src_path`, store it in `master`
 * keyed by its resolved path, and walk it for further references.
 *
 * A path already in `master` is left alone. That both keeps one copy of a file
 * reached by several routes and stops a reference cycle: the key is stamped on
 * before the file is walked, so a file that reaches itself finds itself there.
 */
static void add_referenced_file(ryml::Tree& master, const std::string& src_path,
                                const std::string& ref) {
    const std::string path = resolve_path(src_path, ref);
    if (master.find_child(master.root_id(), ryml::to_csubstr(path)) !=
        ryml::NONE)
        return;

    ParsedData* child = static_cast<ParsedData*>(parse_file(path.c_str()));
    if (!child) return;  // reported where the reference is resolved
    ensure_capacity(master, 2);
    size_t dest = master.append_child(master.root_id());
    deep_copy_recursive(master, dest, child->tree, child->tree.root_id());
    // Root has no key, so stamp the path on after the copy
    master.ref(dest) |= ryml::KEY;
    master.set_key(dest, master.to_arena(ryml::to_csubstr(path)));
    add_to_master_tree(master, child->tree, child->tree.root_id(), path);
    delete child;
}

static void add_to_master_tree(ryml::Tree& master, const ryml::Tree& src,
                               size_t node, const std::string& src_path) {
    if (node == ryml::NONE || src.is_val(node)) return;
    for (size_t c = src.first_child(node); c != ryml::NONE;
         c = src.next_sibling(c)) {
        if (src.has_key(c) && src.key(c) == "include" && src.has_val(c)) {
            add_referenced_file(master, src_path,
                                std::string(src.val(c).str, src.val(c).len));
        } else if (src.has_key(c) && src.key(c) == "load" && src.is_seq(c)) {
            // A list of file names, with SELF standing for the naming file
            // itself rather than for a file to read.
            for (size_t e = src.first_child(c); e != ryml::NONE;
                 e = src.next_sibling(e)) {
                if (!src.has_val(e)) continue;
                std::string ref(src.val(e).str, src.val(e).len);
                if (ref != LOAD_SELF) add_referenced_file(master, src_path, ref);
            }
        } else if (src.has_key(c) && src.key(c) == "load" && src.has_val(c)) {
            std::string ref(src.val(c).str, src.val(c).len);
            if (ref != LOAD_SELF) add_referenced_file(master, src_path, ref);
        } else {
            add_to_master_tree(master, src, c, src_path);
        }
    }
}

/**
 * Makes the original lattice. Creates a tree that maps every file the document
 * is built from -- the top-level file and everything it reaches by `include` or
 * `load`, at any depth -- to that file's contents, keyed by its resolved path.
 *
 * `src` is the parsed top-level document, null if it could not be read or
 * parsed; it is freed here. `filename` is the path it is keyed by, which is also
 * what its `include` and `load` references resolve against.
 *
 * If the top-level file cannot be read or is not valid YAML, `parse_error` is
 * set to a human-readable description (with the offending line/column for a
 * syntax error) and the returned tree is an empty MAP. Callers treat a non-empty
 * `parse_error` as a fatal failure: there is no document to expand. A referenced
 * file that cannot be read is not fatal: it is simply missing from the master
 * tree, and reported when the reference to it is resolved.
 */
static YAMLTreeHandle make_original(ParsedData* src,
                                    const std::string& filename,
                                    std::string& parse_error) {
    ParsedData* master = new ParsedData();
    master->tree.rootref() |= ryml::MAP;
    if (src) {
        ensure_capacity(master->tree, 2);
        size_t dest = master->tree.append_child(master->tree.root_id());
        deep_copy_recursive(master->tree, dest, src->tree, src->tree.root_id());
        // Root has no key, so stamp the filename on after the copy
        master->tree.ref(dest) |= ryml::KEY;
        master->tree.set_key(dest,
                             master->tree.to_arena(ryml::to_csubstr(filename)));
        add_to_master_tree(master->tree, src->tree, src->tree.root_id(),
                           filename);
        delete src;
    } else {
        // parse_file recorded why on this thread; nothing else has parsed since.
        parse_error = yaml_last_parse_error();
        if (parse_error.empty()) parse_error = "parse failed";
    }
    return master;
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

YAML_API void free_correspondence_map(struct correspondence_map map) {
    delete[] map.links;
}

}  // extern "C"
