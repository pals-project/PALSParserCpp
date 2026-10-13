#ifndef PALS_UTIL_H
#define PALS_UTIL_H

// Small helpers shared between the PALS expansion pipeline (pals_expand.cpp)
// and PALS name matching / parameter lookup (pals_match.cpp): dotted
// parameter-path utilities and the expr(...) unwrapper. Not part of the public
// C API declared in PALSParserCpp.h.

#include <cstddef>
#include <string>
#include <vector>

#include <ryml.hpp>

// Get the string value of a keyed child of `parent`; "" if not found.
std::string child_val_str(const ryml::Tree& t, size_t parent, const char* key);

// Split a dotted parameter path ("a.b.c") into its components.
std::vector<std::string> split_dots(const std::string& s);

// Walk a dotted parameter path down from an element's definition map. Each
// component is an exact key lookup. Returns the final node, or ryml::NONE if
// any component is missing.
size_t resolve_param_path(const ryml::Tree& t, size_t ele,
                          const std::vector<std::string>& path);

// Trims surrounding whitespace and, if the whole value is `expr(...)`, unwraps
// it. `was_expr` reports whether an `expr(...)` wrapper was present.
std::string strip_expr_wrapper(const std::string& s, bool& was_expr);

// What a PALS name-matching string (match_names syntax, see PALSParserCpp.h)
// selected, reported as the element definitions it matched plus the parameter
// path it named. Elements are listed whether or not they carry that parameter,
// so a caller that *writes* the parameter -- a controller driving it -- can
// create it. Constants, variables and controllers are not considered.
struct ElementMatches {
    std::vector<size_t> elements;   // matched element definition maps
    std::vector<std::string> path;  // dotted parameter path, empty if none named
    bool has_param = false;         // the string named a parameter at all
    bool valid = true;              // false when the string is malformed
};

// Run a name-matching string over the expanded lattice at `root` (ryml::NONE
// for the whole tree). Scoping to the expanded lattice node is what keeps a
// controller from also matching the unexpanded element definitions still
// sitting in `facility`. `extra` are keyed definition maps standing outside any
// line that an unqualified name also reaches (the girders, when a Girder names
// what it supports).
ElementMatches match_element_parameters(const ryml::Tree& t, size_t root,
                                        const std::string& spec,
                                        const std::vector<size_t>& extra = {});

// Like match_element_parameters, but before expansion: `entries` are
// `facility` (or `post_expansion`) list entries, each an anonymous wrapper map
// holding one keyed definition. This is what a pre-expansion `set` acts on, so
// passing only the entries that precede the set is what restricts it to the
// elements defined by that point. An unqualified name reaches the definitions
// the entries hold; a `{branch}>>` qualified one reaches the elements in the
// lines of the BeamLines they define. A `{lattice}>>>` qualifier cannot apply --
// no lattice has been built yet -- and makes the match string invalid here.
ElementMatches match_definition_parameters(const ryml::Tree& t,
                                           const std::vector<size_t>& entries,
                                           const std::string& spec);

#endif  // PALS_UTIL_H
