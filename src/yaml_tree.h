#ifndef YAML_TREE_H
#define YAML_TREE_H

// The concrete YAML tree representation (the type behind the opaque
// YAMLTreeHandle) and its low-level helpers, shared between the generic YAML
// tree wrapper (PALSParserCpp.cpp) and the PALS lattice logic (see
// pals_pipeline.h). This header is not installed and is not part of the
// public C API declared in PALSParserCpp.h.

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <ryml.hpp>

#include "PALSParserCpp.h"

// Underlying object for YAMLTreeHandle. Contains the parsed YAML tree and the
// string buffer it was parsed from. Nodes are identied by their index (of type
// size_t within its tree.
//
// `provenance` links this tree back to the one it was derived from in the
// original -> combined -> (expanded, full_expanded, adjunct) chain: it maps a
// node id in *this* tree to the node id in the *source* tree it was copied from.
// It is empty for trees that are not derived from another (e.g. `original`). For
// `combined` it maps combined ids -> original ids; for the two expanded trees
// and `adjunct` alike it maps their ids -> combined ids.
struct ParsedData {
    ryml::Tree tree;
    std::string buffer;
    std::map<size_t, size_t> provenance;
    // Controller-driven parameters: a node of this tree -> the `combined`
    // nodes of the `expression`s of every controller driving it, in evaluation
    // order. Kept apart from `provenance` because a parameter may be driven by
    // several controllers at once.
    std::map<size_t, std::vector<size_t>> drivers;
};

#define GET_TREE(handle) (static_cast<ParsedData*>(handle)->tree)

// Grow node pool if nearly full. ryml does not auto-resize.
void ensure_capacity(ryml::Tree& t, size_t needed = 1);

// Cross-tree deep copy: copies type, key, val, and all descendants of src_node
// into the existing dst_node. Strings are copied into dst_t's arena. ryml only
// natively supports copying nodes within the same tree.
void deep_copy_recursive(ryml::Tree& dst_t, size_t dst_node,
                         const ryml::Tree& src_t, size_t src_node);

#endif  // YAML_TREE_H
