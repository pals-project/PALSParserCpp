// Generic YAML tree wrapper: a thin C API over rapidyaml (parse, traverse,
// query, modify, and emit YAML trees). It knows nothing about PALS; the PALS
// lattice logic that builds on it starts in pals_expand.cpp (see
// pals_pipeline.h). Declarations shared between the two are in yaml_tree.h.

#include "PALSParserCpp.h"
#include "yaml_tree.h"

#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// The most recent parse error on this thread, set whenever parse_file() or
// parse_string() fails and read back through yaml_last_parse_error(). Made
// thread-local so concurrent parses do not clobber one another's message.
thread_local std::string g_last_parse_error;

// Thrown by the ryml error callbacks below in place of ryml's default abort(),
// so a malformed document surfaces as a catchable failure (a NULL handle) that
// the host — Julia, say — can report, rather than a signal that kills the whole
// process. Carries ryml's raw message plus the 1-based YAML location, so the
// caller (which still holds the source buffer) can render a source snippet.
// `line`/`col` are ryml::npos when unknown.
struct RymlError : std::runtime_error {
    size_t line;
    size_t col;
    RymlError(ryml::csubstr msg, size_t line_, size_t col_)
        : std::runtime_error(std::string(msg.str, msg.len)),
          line(line_),
          col(col_) {}
};

// ryml requires its error callbacks never return; each throws instead. The
// parse callback reports the location within the YAML source (`ymlloc`), which
// is what pinpoints the offending line for the user.
[[noreturn]] void ryml_error_parse(ryml::csubstr msg,
                                   const ryml::ErrorDataParse& err, void*) {
    throw RymlError(msg, err.ymlloc.line, err.ymlloc.col);
}

[[noreturn]] void ryml_error_basic(ryml::csubstr msg,
                                   const ryml::ErrorDataBasic& err, void*) {
    throw RymlError(msg, err.location.line, err.location.col);
}

// Return 1-based line `n` of `src` (without its trailing newline), or "" if the
// source has fewer than `n` lines. ryml filters scalars in place during a parse
// but never moves the newlines that bound the lines, so scanning the (possibly
// partly-parsed) buffer still recovers the correct line boundaries.
std::string source_line(const std::string& src, size_t n) {
    if (n == 0) return {};
    size_t start = 0;
    for (size_t cur = 1; cur < n; ++cur) {
        size_t nl = src.find('\n', start);
        if (nl == std::string::npos) return {};
        start = nl + 1;
    }
    size_t nl = src.find('\n', start);
    std::string line =
        src.substr(start, nl == std::string::npos ? nl : nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// True when a source line has no non-whitespace content.
bool is_blank_line(const std::string& line) {
    return line.find_first_not_of(" \t") == std::string::npos;
}

// Append a source snippet to `out`: the preceding context, the error line,
// and a caret under the offending column, e.g.
//
//       32 |     - cav
//       33 |         kind: RFCavity
//                        ^
//
// A missing colon is only detectable on the line *after* the omission, so the
// preceding line is shown too — that is where the real fault usually is. When
// that preceding line is blank it is no help, so the scan walks further back to
// the first non-blank line and shows every line from there down to the error,
// e.g. a missing dash reported on line 68 whose real fault is on line 66:
//
//       66 |     - use: "lat1"
//       67 |
//       68 |     ext_line:
//              ^
//
// Does nothing when the location is unknown.
void append_source_context(std::string& out, const std::string& src,
                           size_t line, size_t col) {
    if (line == ryml::npos || line == 0) return;
    const size_t gutter = std::to_string(line).size();
    auto emit_line = [&](size_t n) {
        if (n == 0) return;
        const std::string num = std::to_string(n);
        out += "\n    ";
        out.append(gutter - num.size(), ' ');
        out += num;
        out += " | ";
        out += source_line(src, n);
    };
    if (line > 1) {
        // Walk back over blank preceding lines to the first non-blank one, then
        // show that line and everything between it and the error.
        size_t first = line - 1;
        while (first > 1 && is_blank_line(source_line(src, first))) --first;
        for (size_t n = first; n < line; ++n) emit_line(n);
    }
    emit_line(line);
    if (col != ryml::npos && col >= 1) {
        out += "\n    ";
        out.append(gutter, ' ');
        out += " | ";
        out.append(col - 1, ' ');
        out += '^';
    }
}

// Build the full "line L, column C: <msg>" string, followed by the source
// snippet, from a caught RymlError and the buffer it was parsing.
std::string build_parse_error(const RymlError& e, const std::string& src) {
    std::string out;
    if (e.line != ryml::npos) {
        out = "line " + std::to_string(e.line);
        if (e.col != ryml::npos) out += ", column " + std::to_string(e.col);
        out += ": ";
    }
    out += e.what();
    append_source_context(out, src, e.line, e.col);
    return out;
}

// Install the throwing error callbacks, once, so every parse reports errors by
// exception rather than by aborting. ryml's allocation callbacks are left
// untouched. This runs lazily on the first parse rather than at load time on
// purpose: ryml's global callbacks live in a namespace-scope object whose own
// default (aborting) construction could otherwise run *after* ours in an
// unspecified static-init order and clobber it. Call it before constructing any
// Tree, whose callbacks are copied from the global at construction.
void ensure_throwing_callbacks() {
    static std::once_flag once;
    std::call_once(once, [] {
        ryml::Callbacks cb = ryml::get_callbacks();
        cb.set_error_parse(&ryml_error_parse);
        cb.set_error_basic(&ryml_error_basic);
        ryml::set_callbacks(cb);
    });
}

}  // namespace

// Grow node pool if nearly full. ryml does not auto-resize.
void ensure_capacity(ryml::Tree& t, size_t needed) {
    if (t.size() + needed >= t.capacity()) t.reserve(t.capacity() + 64);
}

// Append or insert a blank child. index=YAML_END means append.
static size_t add_child_at(ryml::Tree& t, size_t parent, size_t index) {
    ensure_capacity(t);
    if (index == YAML_END) return t.append_child(parent);
    size_t num = t.num_children(parent);
    if (index > num) index = num;
    size_t after = (index > 0) ? t.child(parent, index - 1) : ryml::NONE;
    return t.insert_child(parent, after);
}

// Cross-tree deep copy: copies type, key, val, and all descendants of src_node
// into the existing dst_node. Strings are copied into dst_t's arena. ryml only
// natively supports copying nodes within the same tree. Declared in
// yaml_tree.h; also used by the PALS lattice logic.
void deep_copy_recursive(ryml::Tree& dst_t, size_t dst_node,
                         const ryml::Tree& src_t, size_t src_node) {
    std::vector<size_t> src_children;
    for (size_t c = src_t.first_child(src_node); c != ryml::NONE;
         c = src_t.next_sibling(c))
        src_children.push_back(c);

    // Copy type flags (MAP, SEQ, VAL, KEY)
    dst_t.ref(dst_node) |= (src_t.type(src_node) &
                            (ryml::MAP | ryml::SEQ | ryml::VAL | ryml::KEY));

    // Copy key and val
    if (src_t.has_key(src_node)) {
        ryml::csubstr k = src_t.key(src_node);
        dst_t.set_key(dst_node, dst_t.to_arena(k));
    }
    if (src_t.has_val(src_node)) {
        ryml::csubstr v = src_t.val(src_node);
        dst_t.set_val(dst_node, dst_t.to_arena(v));
    }

    // Pre-allocate dst children, then recurse
    std::vector<size_t> dst_children;
    for (size_t i = 0; i < src_children.size(); i++) {
        ensure_capacity(dst_t);
        dst_children.push_back(dst_t.append_child(dst_node));
    }
    for (size_t i = 0; i < src_children.size(); i++)
        deep_copy_recursive(dst_t, dst_children[i], src_t, src_children[i]);
}

extern "C" {

YAML_API YAMLTreeHandle parse_file(const char* filename) {
    ensure_throwing_callbacks();
    g_last_parse_error.clear();
    std::unique_ptr<ParsedData> data(new ParsedData());
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file) {
            g_last_parse_error =
                "could not open file: " +
                std::string(filename ? filename : "(null)");
            return nullptr;
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        data->buffer.resize(size);
        file.read(&data->buffer[0], size);
        // Parse into data->tree (not the returning overload) so the parser
        // inherits the tree's callbacks — the global throwing ones set above.
        // The returning parse_in_place(substr) builds its event handler with
        // ryml's built-in defaults, which abort() on a syntax error.
        ryml::parse_in_place(ryml::to_substr(data->buffer), &data->tree);
        return data.release();
    } catch (const RymlError& e) {
        g_last_parse_error = build_parse_error(e, data->buffer);
        return nullptr;
    } catch (const std::exception& e) {
        g_last_parse_error = e.what();
        return nullptr;
    } catch (...) {
        g_last_parse_error = "unknown parse error";
        return nullptr;
    }
}

YAML_API YAMLTreeHandle parse_string(const char* yaml_str) {
    ensure_throwing_callbacks();
    g_last_parse_error.clear();
    // Explicit null check prevents the segfault
    if (yaml_str == nullptr) {
        g_last_parse_error = "null YAML string";
        return nullptr;
    }

    std::unique_ptr<ParsedData> data(new ParsedData());
    try {
        data->buffer = yaml_str;
        // Parse into data->tree so the parser uses the tree's (global, throwing)
        // callbacks rather than ryml's built-in aborting defaults.
        ryml::parse_in_place(ryml::to_substr(data->buffer), &data->tree);
        return data.release();
    } catch (const RymlError& e) {
        g_last_parse_error = build_parse_error(e, data->buffer);
        return nullptr;
    } catch (const std::exception& e) {
        g_last_parse_error = e.what();
        return nullptr;
    } catch (...) {
        g_last_parse_error = "unknown parse error";
        return nullptr;
    }
}

YAML_API const char* yaml_last_parse_error(void) {
    return g_last_parse_error.c_str();
}

YAML_API YAMLTreeHandle create_empty_tree() {
    ParsedData* data = new ParsedData();
    data->tree.rootref() |= ryml::MAP;
    return data;
}

YAML_API void delete_tree(YAMLTreeHandle tree) {
    delete static_cast<ParsedData*>(tree);
}

YAML_API void remove_node(YAMLTreeHandle tree, YAMLNodeId parent,
                          YAMLNodeId child) {
    if (!tree || child == YAML_NULL_ID) return;
    GET_TREE(tree).remove(child);
}

// --- TRAVERSAL ---

YAML_API YAMLNodeId get_root(YAMLTreeHandle tree) {
    if (!tree) return YAML_NULL_ID;
    return GET_TREE(tree).root_id();
}

YAML_API YAMLNodeId get_parent(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree) return YAML_NULL_ID;
    ryml::Tree& t = GET_TREE(tree);
    if (node == ryml::NONE || !t.has_parent(node)) return YAML_NULL_ID;
    return t.parent(node);
}

YAML_API YAMLNodeId get_child_by_key(YAMLTreeHandle tree, YAMLNodeId parent,
                                     const char* key) {
    if (!tree || !key || parent == YAML_NULL_ID) return YAML_NULL_ID;
    ryml::Tree& t = GET_TREE(tree);
    if (!t.is_map(parent)) return YAML_NULL_ID;
    size_t child = t.find_child(parent, ryml::to_csubstr(key));
    return (child == ryml::NONE) ? YAML_NULL_ID : child;
}

YAML_API YAMLNodeId get_child_by_index(YAMLTreeHandle tree, YAMLNodeId parent,
                                       size_t index) {
    if (!tree || parent == YAML_NULL_ID) return YAML_NULL_ID;
    ryml::Tree& t = GET_TREE(tree);
    if (!(t.is_seq(parent) || t.is_map(parent)) ||
        index >= t.num_children(parent))
        return YAML_NULL_ID;
    return t.child(parent, index);
}

YAML_API size_t get_size(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree || node == YAML_NULL_ID) return 0;
    return GET_TREE(tree).num_children(node);
}

YAML_API char* get_node_key(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree || node == YAML_NULL_ID) return nullptr;
    ryml::Tree& t = GET_TREE(tree);
    if (!t.has_key(node)) return nullptr;
    ryml::csubstr k = t.key(node);
    char* result = new char[k.len + 1];
    memcpy(result, k.str, k.len);
    result[k.len] = '\0';
    return result;
}

// --- TYPE CHECKS ---

YAML_API bool is_map(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree || node == YAML_NULL_ID) return false;
    return GET_TREE(tree).is_map(node);
}

YAML_API bool is_sequence(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree || node == YAML_NULL_ID) return false;
    return GET_TREE(tree).is_seq(node);
}

YAML_API bool is_scalar(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree || node == YAML_NULL_ID) return false;
    return GET_TREE(tree).is_val(node);
}

// --- READING VALUES ---

YAML_API char* as_string(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree || node == YAML_NULL_ID) return nullptr;
    ryml::Tree& t = GET_TREE(tree);
    if (!t.has_val(node)) return nullptr;
    ryml::csubstr v = t.val(node);
    char* result = new char[v.len + 1];
    memcpy(result, v.str, v.len);
    result[v.len] = '\0';
    return result;
}

// --- MODIFICATION ---

YAML_API YAMLNodeId add_scalar(YAMLTreeHandle tree, YAMLNodeId parent,
                               const char* key, const char* value,
                               size_t index) {
    if (!tree || !value || parent == YAML_NULL_ID) return YAML_NULL_ID;
    ryml::Tree& t = GET_TREE(tree);
    size_t id = add_child_at(t, parent, index);
    if (t.is_map(parent) && key) {
        t.ref(id) |= ryml::KEY | ryml::VAL;
        t.set_key(id, t.to_arena(ryml::to_csubstr(key)));
        t.set_val(id, t.to_arena(ryml::to_csubstr(value)));
    } else {
        t.to_val(id, t.to_arena(ryml::to_csubstr(value)));
    }
    return id;
}

YAML_API YAMLNodeId add_map(YAMLTreeHandle tree, YAMLNodeId parent,
                            const char* key, size_t index) {
    if (!tree || parent == YAML_NULL_ID) return YAML_NULL_ID;
    ryml::Tree& t = GET_TREE(tree);
    size_t id = add_child_at(t, parent, index);
    if (t.is_map(parent) && key)
        t.to_map(id, t.to_arena(ryml::to_csubstr(key)));
    else
        t.to_map(id);
    return id;
}

YAML_API YAMLNodeId add_sequence(YAMLTreeHandle tree, YAMLNodeId parent,
                                 const char* key, size_t index) {
    if (!tree || parent == YAML_NULL_ID) return YAML_NULL_ID;
    ryml::Tree& t = GET_TREE(tree);
    size_t id = add_child_at(t, parent, index);
    if (t.is_map(parent) && key)
        t.to_seq(id, t.to_arena(ryml::to_csubstr(key)));
    else
        t.to_seq(id);
    return id;
}

YAML_API void set_scalar(YAMLTreeHandle tree, YAMLNodeId node,
                         const char* value) {
    if (!tree || !value || node == YAML_NULL_ID) return;
    ryml::Tree& t = GET_TREE(tree);
    t.ref(node) |= ryml::VAL;
    t.set_val(node, t.to_arena(ryml::to_csubstr(value)));
}

YAML_API void set_node_key(YAMLTreeHandle tree, YAMLNodeId node,
                           const char* key) {
    if (!tree || !key || node == YAML_NULL_ID) return;
    ryml::Tree& t = GET_TREE(tree);
    t.ref(node) |= ryml::KEY;
    t.set_key(node, t.to_arena(ryml::to_csubstr(key)));
}

YAML_API void deep_copy_node(YAMLTreeHandle dst_tree, YAMLNodeId dst_node,
                             YAMLTreeHandle src_tree, YAMLNodeId src_node) {
    if (!dst_tree || !src_tree) return;
    if (dst_node == YAML_NULL_ID || src_node == YAML_NULL_ID) return;
    ryml::Tree& dt = GET_TREE(dst_tree);
    const ryml::Tree& st = GET_TREE(src_tree);
    ensure_capacity(dt, st.num_children(src_node) + 1);
    dt.duplicate_contents(&st, src_node, dst_node);
}

YAML_API void deep_copy_children(YAMLTreeHandle dst_tree, YAMLNodeId dst_node,
                                 YAMLTreeHandle src_tree, YAMLNodeId src_node,
                                 size_t index) {
    if (!dst_tree || !src_tree) return;
    if (dst_node == YAML_NULL_ID || src_node == YAML_NULL_ID) return;
    ryml::Tree& dt = GET_TREE(dst_tree);
    const ryml::Tree& st = GET_TREE(src_tree);
    size_t after;
    if (index == YAML_END)
        after = dt.last_child(dst_node);
    else if (index == 0)
        after = ryml::NONE;
    else
        after = dt.child(dst_node, index - 1);
    ensure_capacity(dt, st.num_children(src_node) + 1);
    dt.duplicate_children(&st, src_node, dst_node, after);
}

// --- EMITTING & UTILS ---

YAML_API char* node_to_string(YAMLTreeHandle tree, YAMLNodeId node) {
    if (!tree) return nullptr;
    ryml::Tree& t = GET_TREE(tree);
    if (node == ryml::NONE || node >= t.capacity()) return nullptr;
    std::stringstream ss;
    ss << t.ref(node);
    std::string str = ss.str();
    char* result = new char[str.length() + 1];
    strcpy(result, str.c_str());
    return result;
}

YAML_API char* tree_to_string(YAMLTreeHandle tree) {
    return node_to_string(tree, get_root(tree));
}

YAML_API bool write_file(YAMLTreeHandle tree, const char* filename) {
    if (!tree || !filename) return false;
    try {
        std::ofstream fout(filename);
        if (!fout) return false;
        fout << GET_TREE(tree);
        return true;
    } catch (...) {
        return false;
    }
}

YAML_API void yaml_free_string(char* str) { delete[] str; }

}  // extern "C"
