// Tree helpers shared by the stages of the PALS expansion pipeline. See
// pals_tree.h for what each does.

#include "pals_tree.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "yaml_tree.h"

namespace pals {

// ------------------------------------------------------------
// Reading
// ------------------------------------------------------------

std::string key_str(const ryml::Tree& t, size_t node) {
    if (node == ryml::NONE || !t.has_key(node)) return "";
    return std::string(t.key(node).str, t.key(node).len);
}

std::string val_str(const ryml::Tree& t, size_t node) {
    if (node == ryml::NONE || !t.has_val(node)) return "";
    return std::string(t.val(node).str, t.val(node).len);
}

std::string trim(const std::string& s, const char* ws) {
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
        s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

std::string format_double(double v) {
    char buf[64];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) return std::string(buf);
    }
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

bool parse_num(const std::string& s, double& out) {
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

bool get_num_child(const ryml::Tree& t, size_t parent, const char* key,
                   double& out) {
    if (parent == ryml::NONE || !t.is_map(parent)) return false;
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c == ryml::NONE || !t.has_val(c)) return false;
    return parse_num(val_str(t, c), out);
}

bool get_str_child(const ryml::Tree& t, size_t parent, const char* key,
                   std::string& out) {
    if (parent == ryml::NONE || !t.is_map(parent)) return false;
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c == ryml::NONE || !t.has_val(c)) return false;
    std::string v = val_str(t, c);
    if (v.find_first_not_of(" \t\r\n") == std::string::npos) return false;
    v = unquote(trim(v));
    if (v.empty()) return false;
    out = v;
    return true;
}

bool is_true_flag(const std::string& v) {
    return v == "true" || v == "True" || v == "TRUE" || v == "1";
}

bool parse_node_id(const std::string& s, size_t& out) {
    try {
        out = static_cast<size_t>(std::stoull(s));
        return true;
    } catch (...) {
        return false;
    }
}

std::string ele_name(const ryml::Tree& t, size_t ele) {
    return t.has_key(ele) ? key_str(t, ele) : "<element>";
}

// ------------------------------------------------------------
// Writing
// ------------------------------------------------------------

size_t append_keyval(ryml::Tree& t, size_t parent, const char* key,
                     const std::string& val) {
    ensure_capacity(t, 2);
    size_t c = t.append_child(parent);
    t.ref(c) |= ryml::KEY | ryml::VAL;
    t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    t.set_val(c, t.to_arena(ryml::to_csubstr(val)));
    return c;
}

size_t find_or_add_seq_child(ryml::Tree& t, size_t parent, const char* key) {
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c != ryml::NONE) return c;
    ensure_capacity(t, 2);
    c = t.append_child(parent);
    t.ref(c) |= ryml::KEY | ryml::SEQ;
    t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    return c;
}

size_t find_or_add_map_child(ryml::Tree& t, size_t parent, const char* key) {
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

// The keyed scalar child `key` of `parent`, created (with no value yet) if
// absent. The three setters below differ only in what they then write.
static size_t find_or_add_val_child(ryml::Tree& t, size_t parent,
                                    const char* key) {
    size_t c = t.find_child(parent, ryml::to_csubstr(key));
    if (c != ryml::NONE) return c;
    ensure_capacity(t, 2);
    c = t.append_child(parent);
    t.ref(c) |= ryml::KEY | ryml::VAL;
    t.set_key(c, t.to_arena(ryml::to_csubstr(key)));
    return c;
}

void set_num_child(ryml::Tree& t, size_t parent, const char* key, double v) {
    size_t c = find_or_add_val_child(t, parent, key);
    t.set_val(c, t.to_arena(ryml::to_csubstr(format_double(v))));
}

void set_str_child(ryml::Tree& t, size_t parent, const char* key,
                   const std::string& v) {
    size_t c = find_or_add_val_child(t, parent, key);
    t.set_val(c, t.to_arena(ryml::to_csubstr(v)));
    t.set_val_style(c, ryml::VAL_DQUO);
}

void set_plain_child(ryml::Tree& t, size_t parent, const char* key,
                     const std::string& v) {
    size_t c = find_or_add_val_child(t, parent, key);
    t.set_val(c, t.to_arena(ryml::to_csubstr(v)));
}

void set_scalar_num(ryml::Tree& t, size_t node, double v) {
    t.set_val(node, t.to_arena(ryml::to_csubstr(format_double(v))));
}

// ------------------------------------------------------------
// Provenance-tracked copies
// ------------------------------------------------------------

void deep_copy_tracked_except(ryml::Tree& dst_t, size_t dst_node,
                              const ryml::Tree& src_t, size_t src_node,
                              size_t skip, Provenance& prov) {
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

void deep_copy_tracked(ryml::Tree& dst_t, size_t dst_node,
                       const ryml::Tree& src_t, size_t src_node,
                       Provenance& prov) {
    deep_copy_tracked_except(dst_t, dst_node, src_t, src_node, ryml::NONE, prov);
}

void chain_prov(Provenance& prov, const Provenance& via) {
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

void erase_prov_subtree(const ryml::Tree& t, size_t node, Provenance& prov) {
    if (node == ryml::NONE) return;
    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        erase_prov_subtree(t, c, prov);
    prov.erase(node);
}

void remove_tracked(ryml::Tree& t, size_t node, Provenance& prov) {
    erase_prov_subtree(t, node, prov);
    t.remove(node);
}

void link_to_writer(size_t written, size_t writer, Provenance& prov) {
    if (written == ryml::NONE) return;
    auto w = writer == ryml::NONE ? prov.end() : prov.find(writer);
    if (w == prov.end())
        prov.erase(written);
    else
        prov[written] = w->second;
}

// After a within-tree duplicate produced `new_node` from `src_node` (an
// isomorphic subtree), walk both in parallel and give each new node the same
// provenance as its template -- i.e. new copies point at the same source node
// their template did. Nodes whose template had no provenance stay unmapped.
static void assign_prov_parallel(const ryml::Tree& t, size_t new_node,
                                 size_t src_node, Provenance& prov) {
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

size_t duplicate_tracked(ryml::Tree& t, size_t src, size_t parent,
                         size_t after, Provenance& prov) {
    size_t nw = t.duplicate(src, parent, after);
    assign_prov_parallel(t, nw, src, prov);
    return nw;
}

void duplicate_children_no_rep_tracked(ryml::Tree& t, size_t src, size_t dst,
                                       size_t after, Provenance& prov) {
    for (size_t c = t.first_child(src); c != ryml::NONE;
         c = t.next_sibling(c)) {
        if (t.has_key(c) && t.find_child(dst, t.key(c)) != ryml::NONE)
            continue;  // key already present -- do not overwrite
        after = duplicate_tracked(t, c, dst, after, prov);
    }
}

void merge_children_no_rep_tracked(ryml::Tree& t, size_t src, size_t dst,
                                   Provenance& prov) {
    size_t after = ryml::NONE;
    for (size_t c = t.first_child(src); c != ryml::NONE;
         c = t.next_sibling(c)) {
        if (!t.has_key(c)) continue;
        size_t have = t.find_child(dst, t.key(c));
        if (have == ryml::NONE) {
            ensure_capacity(t, 2);
            after = duplicate_tracked(t, c, dst, after, prov);
        } else if (t.is_map(have) && t.is_map(c)) {
            merge_children_no_rep_tracked(t, c, have, prov);
        }
    }
}

// ------------------------------------------------------------
// Document structure
// ------------------------------------------------------------

void make_ele_map(std::map<std::string, size_t>& emap, const ryml::Tree& t,
                  size_t node) {
    if (node == ryml::NONE || t.is_val(node)) return;

    // A map from a name to its properties, where the properties contain
    // "kind", is an element definition.
    if (t.is_map(node)) {
        size_t first = t.first_child(node);
        if (first != ryml::NONE && t.has_key(first) && t.is_map(first) &&
            t.has_child(first, ryml::to_csubstr("kind")))
            emap.emplace(key_str(t, first), first);
    }

    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        make_ele_map(emap, t, c);
}

size_t find_pals_list(const ryml::Tree& t, const char* key) {
    size_t pals = t.find_child(t.root_id(), ryml::to_csubstr("PALS"));
    if (pals == ryml::NONE) return ryml::NONE;
    size_t l = t.find_child(pals, ryml::to_csubstr(key));
    return (l != ryml::NONE && t.is_seq(l)) ? l : ryml::NONE;
}

std::vector<size_t> pals_list_entries(const ryml::Tree& t, const char* key) {
    std::vector<size_t> out;
    size_t l = find_pals_list(t, key);
    if (l == ryml::NONE) return out;
    for (size_t e = t.first_child(l); e != ryml::NONE; e = t.next_sibling(e))
        out.push_back(e);
    return out;
}

size_t entry_content(const ryml::Tree& t, size_t entry) {
    if (entry == ryml::NONE || !t.is_map(entry)) return ryml::NONE;
    size_t c = t.first_child(entry);
    return (c != ryml::NONE && t.has_key(c)) ? c : ryml::NONE;
}

size_t lattice_branches(const ryml::Tree& t, size_t lat_node) {
    if (lat_node == ryml::NONE) return ryml::NONE;
    size_t b = t.find_child(lat_node, ryml::to_csubstr("branches"));
    return (b != ryml::NONE && t.is_seq(b)) ? b : ryml::NONE;
}

std::vector<size_t> branch_nodes(const ryml::Tree& t, size_t lat_node) {
    std::vector<size_t> out;
    size_t branches = lattice_branches(t, lat_node);
    if (branches == ryml::NONE) return out;
    for (size_t entry = t.first_child(branches); entry != ryml::NONE;
         entry = t.next_sibling(entry)) {
        if (!t.is_map(entry)) continue;
        size_t branch = t.first_child(entry);
        if (branch != ryml::NONE && t.is_map(branch)) out.push_back(branch);
    }
    return out;
}

size_t branch_line(const ryml::Tree& t, size_t branch) {
    size_t line = t.find_child(branch, ryml::to_csubstr("line"));
    return (line != ryml::NONE && t.is_seq(line)) ? line : ryml::NONE;
}

size_t line_entry_element(const ryml::Tree& t, size_t entry) {
    if (!t.is_map(entry)) return ryml::NONE;
    size_t def = t.first_child(entry);
    return (def != ryml::NONE && t.has_key(def)) ? def : ryml::NONE;
}

}  // namespace pals
