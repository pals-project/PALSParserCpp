#ifndef PALS_TREE_H
#define PALS_TREE_H

// Tree helpers shared by the stages of the PALS expansion pipeline (see
// pals_pipeline.h): reading and writing the scalars of a ryml tree, copying
// subtrees while recording provenance, and finding the parts of a PALS document
// -- its lists, a lattice's branches, a branch's line -- by their fixed shape.
// Not part of the public C API declared in PALSParserCpp.h.

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <ryml.hpp>

namespace pals {

// A node id in one tree -> the node id in the tree it was derived from. See
// ParsedData::provenance (yaml_tree.h).
using Provenance = std::map<size_t, size_t>;

// ------------------------------------------------------------
// Reading
// ------------------------------------------------------------

// The key / value text of a node, or "" when it has none.
std::string key_str(const ryml::Tree& t, size_t node);
std::string val_str(const ryml::Tree& t, size_t node);

// `s` with the characters of `ws` removed from both ends.
std::string trim(const std::string& s, const char* ws = " \t\r\n");

// `s` with one pair of matching surrounding quotes ('...' or "...") removed.
std::string unquote(const std::string& s);

// Shortest decimal string that round-trips back to `v` (so integers stay
// integer-looking and no precision is lost).
std::string format_double(double v);

// Parse the whole of `s` (bar trailing whitespace) as a finite double. Values
// in the expanded tree are already evaluated to plain numbers by the time this
// is used on them.
bool parse_num(const std::string& s, double& out);

// Numeric value of a keyed child, if present and numeric.
bool get_num_child(const ryml::Tree& t, size_t parent, const char* key,
                   double& out);

// String value of a keyed child, trimmed and with any surrounding quotes
// stripped; false (and `out` untouched) if absent or empty.
bool get_str_child(const ryml::Tree& t, size_t parent, const char* key,
                   std::string& out);

// True for a YAML scalar meaning boolean true (`multipass: true`, `True`, `1`).
bool is_true_flag(const std::string& v);

// A node id held as text (ForkP.destination_pointer). False if `s` is not one.
bool parse_node_id(const std::string& s, size_t& out);

// The name of an element (its map key), or a placeholder for the unkeyed case.
std::string ele_name(const ryml::Tree& t, size_t ele);

// ------------------------------------------------------------
// Writing
// ------------------------------------------------------------

// Append a keyed scalar child `key: val` to `parent`, returning it.
size_t append_keyval(ryml::Tree& t, size_t parent, const char* key,
                     const std::string& val);

// A keyed sequence child of `parent`, created (empty) if absent.
size_t find_or_add_seq_child(ryml::Tree& t, size_t parent, const char* key);

// A keyed map child of `parent`, created (empty) if absent. An existing scalar
// placeholder of the same key is converted to a map in place.
size_t find_or_add_map_child(ryml::Tree& t, size_t parent, const char* key);

// Set (or create) a keyed scalar child to the shortest round-tripping decimal.
void set_num_child(ryml::Tree& t, size_t parent, const char* key, double v);

// Set (or create) a keyed scalar child to a string. Double-quoted so a leading
// '#' in a species name survives YAML's comment rule on re-emit.
void set_str_child(ryml::Tree& t, size_t parent, const char* key,
                   const std::string& v);

// Set (or create) a keyed scalar child to a bare (unquoted) token. Used for
// enum and boolean defaults (e.g. `cavity_type: STANDING_WAVE`,
// `aperture_active: true`), which are plain YAML scalars, not strings.
void set_plain_child(ryml::Tree& t, size_t parent, const char* key,
                     const std::string& v);

// Overwrite a scalar value node in place with a number.
void set_scalar_num(ryml::Tree& t, size_t node, double v);

// ------------------------------------------------------------
// Provenance-tracked copies
// ------------------------------------------------------------
//
// These mirror the plain copy/duplicate routines but record, for every node
// they create, which source node it came from. The recorded map (`prov`) is
// what lets build_correspondence_map() link a node across the trees.

// Cross-tree deep copy (like deep_copy_recursive) that records
// prov[dst_node] = src_node for every copied node. The destination root keeps
// no key even if src_node has one (a tree root cannot be keyed). `skip` names a
// source node to leave out along with its descendants (ryml::NONE copies
// everything); it is how the adjunct tree is built as the document minus the
// lattice that went into the expanded tree.
void deep_copy_tracked_except(ryml::Tree& dst_t, size_t dst_node,
                              const ryml::Tree& src_t, size_t src_node,
                              size_t skip, Provenance& prov);

void deep_copy_tracked(ryml::Tree& dst_t, size_t dst_node,
                       const ryml::Tree& src_t, size_t src_node,
                       Provenance& prov);

// Rewrite `prov` (ids -> ids in some intermediate tree) so that it points at
// whatever that intermediate tree was itself derived from, using its own
// provenance. This is what lets `expanded` and `adjunct` be cut out of the
// temporary work tree and still record provenance straight back to `combined`:
// the work tree is discarded, so links through it would dangle. Nodes with no
// entry in `via` (created during expansion, e.g. `destination_pointer`) have
// no source and drop out.
void chain_prov(Provenance& prov, const Provenance& via);

// Remove every entry for `node` and its descendants from `prov`. Call this
// before ryml removes a subtree, so freed ids that get reused later cannot
// carry a stale provenance link.
void erase_prov_subtree(const ryml::Tree& t, size_t node, Provenance& prov);

// Remove `node` and its subtree from the tree and from `prov` together.
void remove_tracked(ryml::Tree& t, size_t node, Provenance& prov);

// Link a parameter a `set` or a controller has written (`written`) back to the
// node holding the expression that wrote it (`writer`): the parameter's value
// now comes from there, and what it held before is no longer relevant. The
// correspondence map then leads from the parameter to that expression, so a
// program can tell, for instance, whether it was delayed. A writer with no
// provenance of its own leaves the parameter unlinked.
void link_to_writer(size_t written, size_t writer, Provenance& prov);

// Within-tree duplicate of `src` under `parent` (after `after`), recording
// provenance for the whole duplicated subtree.
size_t duplicate_tracked(ryml::Tree& t, size_t src, size_t parent,
                         size_t after, Provenance& prov);

// Provenance-tracked version of ryml's duplicate_children_no_rep: copies the
// children of `src` into `dst` (appending after `after`) but skips any child
// whose key already exists in `dst`. Used for `inherit`.
void duplicate_children_no_rep_tracked(ryml::Tree& t, size_t src, size_t dst,
                                       size_t after, Provenance& prov);

// Like duplicate_children_no_rep_tracked, but a map present on both sides is
// merged key by key rather than kept whole, so an instance that sets one
// parameter of a group (`SolenoidP: {Ksol: 0.3}`) keeps the group's others.
// The keys brought in go ahead of the receiver's own, in the source's order.
void merge_children_no_rep_tracked(ryml::Tree& t, size_t src, size_t dst,
                                   Provenance& prov);

// ------------------------------------------------------------
// Document structure
// ------------------------------------------------------------

// Build a name->node map for all elements that have a "kind" key.
void make_ele_map(std::map<std::string, size_t>& emap, const ryml::Tree& t,
                  size_t node);

// The keyed list `key` (`facility`, `post_expansion`) of the PALS node, or
// ryml::NONE. Includes have already been spliced into it in place, so it holds
// every node of every file, in order.
size_t find_pals_list(const ryml::Tree& t, const char* key);

// The entries of one of the PALS lists, in order.
std::vector<size_t> pals_list_entries(const ryml::Tree& t, const char* key);

// The keyed child of a list entry (the entry is an anonymous wrapper map
// holding one keyed node), or ryml::NONE for a bare scalar entry.
size_t entry_content(const ryml::Tree& t, size_t entry);

// The `branches` sequence of the lattice `lat_node`, or ryml::NONE.
size_t lattice_branches(const ryml::Tree& t, size_t lat_node);

// The branches of the lattice `lat_node`, in order: the keyed map each
// `branches` entry wraps. Both the written form (`- name: {...}`) and a
// substituted bare name land as a map wrapping the single keyed branch node.
std::vector<size_t> branch_nodes(const ryml::Tree& t, size_t lat_node);

// The `line` sequence of a branch (or BeamLine), or ryml::NONE.
size_t branch_line(const ryml::Tree& t, size_t branch);

// The element definition a line entry holds: the keyed first child of its
// anonymous wrapper map. ryml::NONE for a bare (unresolved) name.
size_t line_entry_element(const ryml::Tree& t, size_t entry);

// Visit every element definition of every branch line. `fn(branch_index,
// line_index, def, entry)` receives the element definition and the anonymous
// seq-entry map wrapping it; the indices count every entry, including those
// skipped for holding no definition. The callback may add nodes to the
// element but must not remove any: the walk holds sibling ids across the call.
template <typename F>
void for_each_line_element(const ryml::Tree& t, size_t lat_node, F fn) {
    size_t branches = lattice_branches(t, lat_node);
    if (branches == ryml::NONE) return;

    size_t b = 0;
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry), ++b) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch == ryml::NONE || !t.is_map(branch)) continue;
        size_t line = branch_line(t, branch);
        if (line == ryml::NONE) continue;

        size_t l = 0;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le), ++l) {
            size_t def = line_entry_element(t, le);
            if (def != ryml::NONE) fn(b, l, def, le);
        }
    }
}

// Visit the `name: value` pairs of a node written either as a map or, in the
// compact form, as a sequence of single-key maps. `fn(kv)` receives each child
// of the map, or each child of each sequence entry, unfiltered.
template <typename F>
void for_each_pair(const ryml::Tree& t, size_t node, F fn) {
    if (node == ryml::NONE) return;
    if (t.is_map(node)) {
        for (size_t kv = t.first_child(node); kv != ryml::NONE;
             kv = t.next_sibling(kv))
            fn(kv);
    } else if (t.is_seq(node)) {
        for (size_t el = t.first_child(node); el != ryml::NONE;
             el = t.next_sibling(el))
            for (size_t kv = t.first_child(el); kv != ryml::NONE;
                 kv = t.next_sibling(kv))
                fn(kv);
    }
}

}  // namespace pals

#endif  // PALS_TREE_H
