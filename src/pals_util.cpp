// Small helpers shared between pals_expand.cpp and pals_match.cpp. See
// pals_util.h for what each does and why they are shared.

#include "pals_util.h"

#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

std::string child_val_str(const ryml::Tree& t, size_t parent, const char* key) {
    // find_child requires a map: a group written as a list or a scalar (a
    // malformed `ForkP:`, say) simply has no such key.
    if (parent == ryml::NONE || !t.is_map(parent)) return "";
    size_t id = t.find_child(parent, ryml::to_csubstr(key));
    if (id == ryml::NONE || !t.has_val(id)) return "";
    return std::string(t.val(id).str, t.val(id).len);
}

std::vector<std::string> split_dots(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

size_t resolve_param_path(const ryml::Tree& t, size_t ele,
                          const std::vector<std::string>& path) {
    size_t cur = ele;
    for (const std::string& comp : path) {
        if (cur == ryml::NONE || !t.is_map(cur)) return ryml::NONE;
        cur = t.find_child(cur, ryml::to_csubstr(comp));
    }
    return cur;
}

std::string strip_expr_wrapper(const std::string& s, bool& was_expr) {
    was_expr = false;
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::string t = s.substr(a, b - a + 1);
    // `expr` may be followed by whitespace before its `(`.
    const std::string pre = "expr";
    if (t.compare(0, pre.size(), pre) != 0) return t;
    size_t open = t.find_first_not_of(" \t", pre.size());
    if (open == std::string::npos || t[open] != '(' || t.back() != ')')
        return t;
    // The wrapper must enclose the entire expression (s:expressions): the `(`
    // after `expr` has to close at the very end, so `expr(a) + expr(b)` is not
    // a wrapped expression -- it is left whole, and the evaluator reports the
    // misplaced `expr`.
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i] == '(') ++depth;
        else if (t[i] == ')' && --depth == 0) {
            if (i != t.size() - 1) return t;
            break;
        }
    }
    was_expr = true;
    return t.substr(open + 1, t.size() - open - 2);
}
