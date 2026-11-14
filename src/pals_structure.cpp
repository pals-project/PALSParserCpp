// Structural expansion of the root lattice: substituting definitions for the
// names that use them, unrolling `repeat`s, splicing sub-lines, copying
// `inherit`s, building the branches Forks lead to, and numbering multipass
// elements. See pals_pipeline.h for where this sits in the pipeline.

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

size_t inherit_source(const ryml::Tree& t,
                      const std::map<std::string, size_t>& emap,
                      const std::string& name) {
    size_t gt = name.find('>');
    auto it = emap.find(gt == std::string::npos ? name : name.substr(0, gt));
    if (it == emap.end()) return ryml::NONE;
    if (gt == std::string::npos) return it->second;
    size_t n = it->second;
    for (const std::string& k : split_dots(name.substr(gt + 1))) {
        if (k.empty() || !t.is_map(n)) return ryml::NONE;
        n = t.find_child(n, ryml::to_csubstr(k));
        if (n == ryml::NONE) return ryml::NONE;
    }
    return t.is_map(n) ? n : ryml::NONE;
}

// True if the keyed map `node` is a `line` or `elements` item that uses, but
// does not define, an element: it has neither a `kind` nor an `inherit`.
static bool is_use_item(const ryml::Tree& t, size_t node) {
    if (t.find_child(node, ryml::to_csubstr("kind")) != ryml::NONE ||
        t.find_child(node, ryml::to_csubstr("inherit")) != ryml::NONE)
        return false;
    size_t wrapper = t.parent(node);
    if (wrapper == ryml::NONE || !t.is_map(wrapper) || t.has_key(wrapper))
        return false;
    size_t seq = t.parent(wrapper);
    if (seq == ryml::NONE || !t.is_seq(seq) || !t.has_key(seq)) return false;
    return t.key(seq) == ryml::to_csubstr("line") ||
           t.key(seq) == ryml::to_csubstr("elements");
}

// Find an element named `name` within a line sequence. The element can be just
// the scalar name or defined as a map.
static size_t find_in_line(const ryml::Tree& t, size_t line,
                           const std::string& name) {
    for (size_t c = t.first_child(line); c != ryml::NONE;
         c = t.next_sibling(c)) {
        if (t.is_val(c) && val_str(t, c) == name) return c;
        if (t.is_map(c)) {
            size_t entry = t.first_child(c);
            if (entry != ryml::NONE && t.has_key(entry) &&
                key_str(t, entry) == name)
                return entry;
        }
    }
    return ryml::NONE;
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
    append_keyval(t, ele, "multipass_index", std::to_string(idx));
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

// Parse a `repeat` count. stoll stops at the first character it cannot use and
// reports how far it got, so the whole value has to be consumed: "3x" is a
// typo, not a count of 3.
static bool parse_repeat_count(const std::string& text, long long& count) {
    try {
        size_t used = 0;
        count = std::stoll(text, &used);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

// ------------------------------------------------------------
// The expansion walk
// ------------------------------------------------------------

// The state one expansion of a lattice carries through its recursion.
struct Expander {
    ryml::Tree& t;
    std::map<std::string, size_t>& emap;  // definition name -> node
    Provenance& prov;
    ProblemList& problems;
    // Per multipass line definition, how many instances of that line have been
    // spliced so far. Expansion visits them in traversal order, so the count is
    // the pass number to stamp on the next instance.
    std::map<size_t, int> mp_pass;
    // Branches a Fork already expanded. See handle_fork.
    std::set<size_t> done;

    void expand(size_t node, size_t branches);
    void expand_seq_range(size_t seq, size_t first, size_t stop,
                          size_t branches);
    bool expand_repeat(size_t seq, size_t child, size_t entry, size_t next,
                       size_t branches);
    void splice_subline(size_t node, size_t def, size_t def_line,
                        size_t branches);
    void handle_fork(size_t fork_node, size_t branches);
};

/**
 * Perform lattice expansion on `node`.
 * 1. Substitute scalar elements with their full definition taken from emap.
 * 2. Beamlines that contain "repeat: n" have their contents repeated |n| times,
 *    each copy expanded in turn like any other entry of the enclosing line. A
 *    negative n reflects: the repeated elements come out in reverse order.
 * 3. Elements that contain "inherit: ancestor" have the contents of ancestor
 *    copied into element.
 * 4. A beamline referenced by bare name inside a `line:` is a sub-line: its
 *    `line:` contents are spliced directly into the enclosing line. When the
 *    sub-line is `multipass`, the spliced run is stamped with its pass number.
 *
 * `branches` is the `branches` sequence of the lattice being expanded, which a
 * Fork appends the branches it creates to.
 */
void Expander::expand(size_t node, size_t branches) {
    if (node == ryml::NONE) return;
    // A branch a Fork built was expanded eagerly, at the point the Fork had to
    // look inside it for its destination element. Expansion is not idempotent
    // (a second pass re-runs the Forks it contains), so it is only ever done
    // once. See handle_fork.
    if (done.count(node)) return;

    // Sequence -- handle 'repeat'
    if (t.is_seq(node)) {
        expand_seq_range(node, t.first_child(node), ryml::NONE, branches);
        return;
    }

    // Standalone scalar value
    if (t.is_val(node) && !t.has_key(node)) {
        std::string name = val_str(t, node);
        size_t p = t.parent(node);
        auto it = emap.find(name);
        if (it == emap.end()) {
            // A bare entry in a `line:` or `branches:` sequence names an element
            // or beamline; if it is not defined, the reference is dangling.
            std::string pk = key_str(t, p);
            if (pk == "line" || pk == "branches")
                add_problem(problems, "reference to undefined element or line '" +
                                          name + "'");
            return;
        }
        size_t def = it->second;
        size_t def_line = branch_line(t, def);

        // Sub-line: a beamline referenced by bare name inside a `line:`
        // contributes its `line:` contents directly to the enclosing line rather
        // than surviving as a nested BeamLine. (A bare name in a `branches:`
        // sequence is a branch, not a sub-line -- it falls through to plain
        // substitution and is stripped of its kind later.)
        if (key_str(t, p) == "line" &&
            child_val_str(t, def, "kind") == "BeamLine" &&
            def_line != ryml::NONE) {
            splice_subline(node, def, def_line, branches);
            return;
        }

        // Element (or a beamline outside a `line:`): substitute in place.
        ensure_capacity(t, 2);
        t.change_type(node, ryml::MAP);
        duplicate_tracked(t, def, node, ryml::NONE, prov);
        expand(node, branches);
        return;
    }

    // Map -- handle inherit, detect Lattice/Fork, recurse
    if (t.is_map(node)) {
        // A keyed `line` or `elements` item with no `kind` or `inherit` of its
        // own uses the element it names (beamlines.md): it gets that element's
        // parameters now, at branch expansion, so every set made in `facility`
        // reaches it (names.md). What the item sets itself wins. A name that
        // is a BeamLine is a sub-line, handled with the sequence.
        if (t.has_key(node) && is_use_item(t, node)) {
            auto it = emap.find(key_str(t, node));
            if (it != emap.end() && it->second != node) {
                std::string dk = child_val_str(t, it->second, "kind");
                if (dk != "BeamLine" && dk != "Lattice") {
                    ensure_capacity(t, 2 * t.num_children(it->second) + 2);
                    merge_children_no_rep_tracked(t, it->second, node, prov);
                }
            }
        }

        // Inherit: merge fields from named element (existing keys win)
        size_t inherit_id = t.find_child(node, ryml::to_csubstr("inherit"));
        if (inherit_id != ryml::NONE) {
            std::string name = val_str(t, inherit_id);
            size_t src = inherit_source(t, emap, name);
            if (src != ryml::NONE) {
                ensure_capacity(t, t.num_children(src) + 1);
                duplicate_children_no_rep_tracked(t, src, node, ryml::NONE,
                                                  prov);
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
            handle_fork(node, branches);
        }

        size_t original_size = t.num_children(node);
        size_t c = t.first_child(node);
        for (size_t i = 0; i < original_size && c != ryml::NONE;
             i++, c = t.next_sibling(c))
            expand(c, node_branches);
    }
}

// Splice the sub-line `def` (whose `line` is `def_line`) in place of the bare
// reference `node` to it, and expand the spliced run.
void Expander::splice_subline(size_t node, size_t def, size_t def_line,
                              size_t branches) {
    size_t p = t.parent(node);
    bool multipass = is_true_flag(child_val_str(t, def, "multipass"));
    // Splice the sub-line's entries in front of this reference, then drop the
    // reference itself. `before`/`stop` bracket the spliced run; both are
    // outside it and stay put while it is expanded.
    size_t before = t.prev_sibling(node);
    size_t stop = t.next_sibling(node);
    size_t after = before;
    for (size_t c2 = t.first_child(def_line); c2 != ryml::NONE;
         c2 = t.next_sibling(c2)) {
        ensure_capacity(t, 2);
        after = duplicate_tracked(t, c2, p, after, prov);
    }
    remove_tracked(t, node, prov);

    // Expand the spliced run in place: element references become element maps,
    // and any nested sub-lines flatten (assigning their own, nearer, multipass
    // indices) in turn.
    size_t first =
        (before == ryml::NONE) ? t.first_child(p) : t.next_sibling(before);
    for (size_t cur = first; cur != ryml::NONE && cur != stop;) {
        size_t nx = t.next_sibling(cur);
        expand(cur, branches);
        cur = nx;
    }

    // The spliced run is one instance (one pass) of this multipass sub-line:
    // stamp every entry a nearer multipass sub-line did not already claim with
    // this instance's pass number. Instances of a given line are visited in
    // traversal order, so the running per-definition count is that pass number.
    if (multipass) stamp_multipass_pass(t, first, stop, ++mp_pass[def]);
}

/**
 * Expand the entries [`first`, `stop`) of the sequence `seq` -- `stop` ==
 * ryml::NONE meaning "to the end" -- handling the `repeat` of each entry.
 *
 * Split out of expand() because `repeat` belongs to a line *entry* but is read
 * from the sequence holding it: expanding an entry on its own walks straight
 * past it. The copies a `repeat` splices in are entries of that same sequence
 * and may carry `repeat`s of their own, so they go back through this loop
 * rather than being expanded one at a time.
 */
void Expander::expand_seq_range(size_t seq, size_t first, size_t stop,
                                size_t branches) {
    size_t child = first;
    while (child != ryml::NONE && child != stop) {
        size_t next = t.next_sibling(child);
        if (t.is_map(child) && t.has_children(child)) {
            // first_child because elements are defined as maps with only one
            // key
            size_t entry = t.first_child(child);
            if (t.has_key(entry) && t.is_map(entry) &&
                expand_repeat(seq, child, entry, next, branches)) {
                child = next;
                continue;
            }
        }
        expand(child, branches);
        child = next;
    }
}

// Unroll the `repeat` of the line entry `child` (wrapping the keyed `entry`) of
// the sequence `seq`, whose next sibling is `next`. Returns true when the entry
// has been replaced by its copies, false to have it expanded as written: it has
// no `repeat`, or one that could not be understood.
//
// A negative count is a reflection: |count| copies whose elements come out in
// reverse order (beamlines.md, s:repetition). Reversed order is not direction
// reversal -- that is the separate `direction` component -- so the copies
// themselves are left untouched and only their order is turned around.
bool Expander::expand_repeat(size_t seq, size_t child, size_t entry,
                             size_t next, size_t branches) {
    size_t repeat_id = t.find_child(entry, ryml::to_csubstr("repeat"));
    if (repeat_id == ryml::NONE || !t.has_val(repeat_id)) return false;
    std::string cnt_txt = val_str(t, repeat_id);
    std::string target = key_str(t, entry);

    // Either problem leaves the entry exactly as written. Only a repeat fully
    // understood removes it: dropping an entry on the strength of a count that
    // could not be read would delete part of the lattice, leaving a
    // plausible-looking `line: []` that is only explained by a problem the
    // caller may not check.
    long long count = 0;
    if (!parse_repeat_count(cnt_txt, count)) {
        add_problem(problems, "repeat: invalid count '" + cnt_txt + "' for '" +
                                  target + "'");
        return false;
    }
    auto it = emap.find(target);
    if (it == emap.end()) {
        add_problem(problems,
                    "repeat: beamline '" + target + "' is not defined");
        return false;
    }
    const bool reflect = count < 0;
    const long long copies = reflect ? -count : count;
    size_t def = it->second;
    size_t line_id = t.find_child(def, ryml::to_csubstr("line"));

    // `before`/`next` bracket the run about to be spliced in: both are outside
    // it and stay put while it is built, so they still bracket it once `child`
    // itself is gone.
    size_t before = t.prev_sibling(child);
    size_t after = before;
    // A repeated beamline with a line has that line duplicated `copies` times.
    // Anything else has the line item itself duplicated `copies` times, minus
    // its `repeat`: the copies are what writing the item out `copies` times
    // would give (beamlines.md, s:repetition), so each keeps the values the
    // item sets and gets the rest from the definition when it is expanded.
    if (line_id != ryml::NONE && t.is_seq(line_id)) {
        for (long long r = 0; r < copies; r++)
            for (size_t c2 = t.first_child(line_id); c2 != ryml::NONE;
                 c2 = t.next_sibling(c2)) {
                ensure_capacity(t, 2);
                after = duplicate_tracked(t, c2, seq, after, prov);
            }
    } else {
        for (long long r = 0; r < copies; r++) {
            ensure_capacity(t, 2);
            size_t wrapper = t.insert_child(seq, after);
            t.ref(wrapper) |= ryml::MAP;
            ensure_capacity(t, t.num_children(entry) + 2);
            size_t copy = duplicate_tracked(t, entry, wrapper, ryml::NONE, prov);
            size_t rep = t.find_child(copy, ryml::to_csubstr("repeat"));
            if (rep != ryml::NONE) remove_tracked(t, rep, prov);
            after = wrapper;
        }
    }
    remove_tracked(t, child, prov);

    // Expand the copies just spliced in. They are duplicates of the repeated
    // definition's own entries -- element references, nested sub-lines, and
    // `repeat`s of their own -- so they get exactly the pass this loop would
    // have given them had they been written out by hand. Skipping them left a
    // branch of bare names that extracted as an empty lattice, and reported
    // nothing.
    auto run_start = [&] {
        return (before == ryml::NONE) ? t.first_child(seq)
                                      : t.next_sibling(before);
    };
    expand_seq_range(seq, run_start(), next, branches);

    // A negative count reflects the run just spliced in. Reversing it only now,
    // after the copies are expanded, is what makes this a reversal of
    // *elements*: by this point every sub-line the copies held has been spliced
    // inline, so the run is the flat element list the standard defines the
    // reflection on, and a reflection nested inside this one has already turned
    // its own stretch around.
    if (reflect) {
        std::vector<size_t> run;
        for (size_t cur = run_start(); cur != ryml::NONE && cur != next;
             cur = t.next_sibling(cur))
            run.push_back(cur);
        // Moving each entry in turn to the head of the run leaves them in the
        // opposite order.
        for (size_t i = 1; i < run.size(); i++) t.move(run[i], before);
    }
    return true;
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
void Expander::handle_fork(size_t fork_node, size_t branches) {
    std::string fork_name =
        t.has_key(fork_node) ? key_str(t, fork_node) : "<fork>";
    auto fail = [&](const std::string& why) {
        add_problem(problems, "Fork element '" + fork_name + "': " + why);
    };
    if (branches == ryml::NONE) return fail("no branches to fork into");

    size_t forkp = t.find_child(fork_node, ryml::to_csubstr("ForkP"));
    if (forkp == ryml::NONE) return fail("missing ForkP");
    if (!t.is_map(forkp))
        return fail(
            "ForkP must be a map of parameters (e.g. \"to_line: ...\"), not a "
            "sequence");

    // Only `to_line` is required. `destination_element` defaults to the
    // destination branch's beginning (first) element. `new_branch` selects the
    // action (fork.md, s:fork.params):
    //   * "SELF" (the default when unset): `to_line` must name a BeamLine; a new
    //     branch is created named after that beam line (i.e. `to_line`).
    //   * "null": `to_line` must name an existing branch; no new branch is made,
    //     the Fork just points into it.
    //   * any other value: as SELF, but the new branch takes that name.
    std::string to_line = child_val_str(t, forkp, "to_line");
    if (to_line.empty() || to_line == "null")
        return fail("ForkP is missing the required field 'to_line'");
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
                key_str(t, entry) == to_line) {
                branch_node = entry;
                break;
            }
        }
        if (branch_node == ryml::NONE)
            return fail("new_branch is null, but to_line '" + to_line +
                        "' is not an existing branch");
    } else {
        // SELF or a custom name: instantiate a new branch from the BeamLine.
        auto it = emap.find(to_line);
        if (it == emap.end())
            return fail("to_line '" + to_line + "' is not a defined BeamLine");
        size_t def = it->second;
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
        expand(branch_node, branches);
        done.insert(branch_node);
        created_branch = true;
    }

    // Find to_element within the new branch's line
    size_t line = branch_line(t, branch_node);
    if (line == ryml::NONE)
        return fail("to_line '" + to_line + "' has no line to fork into");
    size_t target;
    if (to_element.empty()) {
        // Default destination is the branch's beginning (first) element. A line
        // entry may be a bare scalar or a keyed map; mirror find_in_line and
        // resolve a map entry to its keyed element.
        target = t.first_child(line);
        if (target != ryml::NONE && t.is_map(target))
            target = t.first_child(target);
        if (target == ryml::NONE)
            return fail("to_line '" + to_line +
                        "' has no beginning element to fork into");
    } else {
        target = find_in_line(t, line, to_element);
        if (target == ryml::NONE)
            return fail("destination_element '" + to_element +
                        "' not found in '" + to_line + "'");
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
            dest_kind != "Fork")
            return fail("destination element '" + key_str(t, target) +
                        "' in '" + to_line + "' has kind '" +
                        (dest_kind.empty() ? "<none>" : dest_kind) +
                        "'; a Fork destination must be a Marker, a "
                        "BeginningEle, or a Fork");
    }

    // Add ForkP.destination_pointer: <node id of target as string>. It sits in
    // the group beside the `to_line`/`destination_element` it resolves and the
    // `forked_to` it will become, not loose on the element.
    append_keyval(t, forkp, "destination_pointer", std::to_string(target));

    // Resolve ForkP.new_branch to the branch this Fork actually instantiated:
    // its name, or the literal `null` when the Fork points into a branch that
    // already existed. This materializes the input form (unset == SELF, or a
    // custom name) into the name the expanded lattice really carries, and is
    // what tells downstream code whether a new branch was made -- reference and
    // floor are propagated to the destination only when it is the beginning
    // element of a *new* branch (fork.md), so the bookkeeper seeds propagation
    // for these and never for a `null` fork into an existing branch.
    set_plain_child(t, forkp, "new_branch",
                    created_branch ? branch_name : "null");
}

// ------------------------------------------------------------
// Before and after the walk
// ------------------------------------------------------------

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
 * the `to_line` definition outright and never arrives here. A bare
 * `- this_line` is a scalar and needs nothing: name substitution already brings
 * the definition in.
 */
static void default_branch_inherit(ryml::Tree& t, size_t lat_node,
                                   const std::map<std::string, size_t>& emap) {
    for (size_t branch : branch_nodes(t, lat_node)) {
        if (!t.has_key(branch)) continue;
        // An explicit `inherit` is the root line, and a branch carrying its own
        // `line` is already the finished article; neither wants a default.
        if (t.find_child(branch, ryml::to_csubstr("inherit")) != ryml::NONE)
            continue;
        if (t.find_child(branch, ryml::to_csubstr("line")) != ryml::NONE)
            continue;
        std::string name = key_str(t, branch);
        // A name that is not defined is left alone: the empty branch it produces
        // is reported by check_branches_expanded, which says so in those terms
        // rather than as a failed inherit.
        if (emap.find(name) == emap.end()) continue;
        append_keyval(t, branch, "inherit", name);
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
    for (size_t branch : branch_nodes(t, lat_node)) {
        if (!t.has_key(branch)) continue;
        size_t line = branch_line(t, branch);
        if (line != ryml::NONE && t.num_children(line) > 0) continue;

        std::string name = key_str(t, branch);
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
 * Anything other than BeamLine is a branch built from a definition that is not
 * a beamline at all. That is a mistake in the file, and leaving the kind in
 * place keeps the evidence of it visible.
 */
static void strip_branch_kinds(ryml::Tree& t, size_t lat_node,
                               Provenance& prov) {
    for (size_t branch : branch_nodes(t, lat_node)) {
        if (child_val_str(t, branch, "kind") != "BeamLine") continue;
        remove_tracked(t, t.find_child(branch, ryml::to_csubstr("kind")), prov);
    }
}

/**
 * Remove the `Placeholder` elements from every branch of the expanded lattice
 * `lat_node`. A Placeholder in the file holds a place during construction and
 * never survives into the expanded lattice (lattice-element-kinds.md,
 * s:placeholder); the one the bookkeeper caps each branch with is added later.
 */
static void remove_placeholders(ryml::Tree& t, size_t lat_node,
                                Provenance& prov) {
    for (size_t branch : branch_nodes(t, lat_node)) {
        size_t line = branch_line(t, branch);
        if (line == ryml::NONE) continue;
        for (size_t le = t.first_child(line); le != ryml::NONE;) {
            size_t next = t.next_sibling(le);
            size_t def = t.is_map(le) ? t.first_child(le) : ryml::NONE;
            if (def != ryml::NONE && t.is_map(def) &&
                child_val_str(t, def, "kind") == "Placeholder")
                remove_tracked(t, le, prov);
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
 * that shared definition -- branches copied from one root map back to the same
 * combined node -- so the per-definition count is the pass number to stamp.
 */
static void number_multipass_branches(ryml::Tree& t, size_t lat_node,
                                      Provenance& prov) {
    std::map<size_t, int> mp_pass;  // root-line definition -> traversals so far
    for (size_t branch : branch_nodes(t, lat_node)) {
        if (!is_true_flag(child_val_str(t, branch, "multipass"))) continue;
        size_t line = branch_line(t, branch);
        if (line == ryml::NONE) continue;
        // Key on the branch's definition. A branch with no recorded provenance
        // (none should occur here) falls back to its own node id, which is
        // unique and so numbers it as a lone first pass.
        auto it = prov.find(branch);
        size_t def_key = (it != prov.end()) ? it->second : branch;
        stamp_multipass_pass(t, t.first_child(line), ryml::NONE,
                             ++mp_pass[def_key]);
    }
}

void expand_lattice(ryml::Tree& t, size_t lat_node,
                    std::map<std::string, size_t>& emap, Provenance& prov,
                    ProblemList& problems) {
    // Before expand, so the root line a branch names only by its own key is in
    // place as an `inherit` for expansion to follow.
    default_branch_inherit(t, lat_node, emap);

    Expander ex{t, emap, prov, problems, {}, {}};
    ex.expand(lat_node, t.find_child(lat_node, ryml::to_csubstr("branches")));

    // After expand, not inside it: a Fork appends branches as it goes, so only
    // once expand has returned does `branches` hold them all.
    strip_branch_kinds(t, lat_node, prov);
    remove_placeholders(t, lat_node, prov);

    // Likewise after expand, so every branch -- root and forked alike -- has
    // had its chance to fill in, and an empty one really is empty.
    check_branches_expanded(t, lat_node, emap, problems);

    // A branch whose root line is itself `multipass` numbers its elements here
    // -- the branch line is not reached through a sub-line flatten, so its
    // multipass indexing cannot happen during expand().
    number_multipass_branches(t, lat_node, prov);
}

// ------------------------------------------------------------
// Finding the lattice
// ------------------------------------------------------------

// Helper for find_lattice: the lattice called `name` under `node`, or, when
// `name` is empty, ryml::NONE with the last lattice seen left in
// `last_lattice` and the last `use` statement seen in `use_name`.
static size_t find_lattice_recursive(const ryml::Tree& t, size_t node,
                                     const std::string& name,
                                     size_t& last_lattice,
                                     std::string& use_name) {
    if (!t.is_map(node) && !t.is_seq(node)) return ryml::NONE;

    for (size_t c = t.first_child(node); c != ryml::NONE;
         c = t.next_sibling(c)) {
        // Track "use" statements -- last one wins
        if (t.has_key(c) && t.key(c) == ryml::to_csubstr("use") &&
            t.has_val(c)) {
            use_name = val_str(t, c);
            continue;
        }

        // A map with kind: Lattice is a lattice.
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

size_t find_lattice(const ryml::Tree& t, const std::string& name) {
    size_t last_lattice = ryml::NONE;
    std::string use_name;

    size_t found =
        find_lattice_recursive(t, t.root_id(), name, last_lattice, use_name);
    if (found != ryml::NONE) return found;
    if (!name.empty()) return ryml::NONE;

    if (!use_name.empty()) return find_lattice(t, use_name);

    return last_lattice;
}

}  // namespace pals
