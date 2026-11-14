#ifndef PALS_PIPELINE_H
#define PALS_PIPELINE_H

// The stages of the PALS expansion pipeline, and what each one hands the next.
// parse_and_expand_PALS (pals_expand.cpp) builds the five trees as a
// derivation chain:
//
//   original --(pals_combine.cpp: splice includes, merge loads)--> combined
//   combined --(work tree)--> expanded, full_expanded, adjunct
//
// and the work tree in the middle is taken through, in order:
//
//   1. pals_evaluate.cpp    check the constants and variables
//   2. pals_sets.cpp        walk `facility`: inherits, immediate expressions,
//                           pre-expansion sets
//   3. pals_structure.cpp   expand the root lattice: substitution, repeats,
//                           sub-lines, inherits, forks, multipass
//   4. pals_evaluate.cpp    evaluate every expression; apply the controllers
//   5. pals_bookkeeper.cpp  walk each branch: reference, floor, s-position,
//                           dependent parameters
//   6. pals_sets.cpp        `post_expansion` sets, then 4 and 5 again
//   7. pals_bookkeeper.cpp  record what each Fork connected to
//
// Each stage keeps its own helpers file-local; only what another stage calls is
// declared here. The tree helpers they all use are in pals_tree.h. Not part of
// the public C API declared in PALSParserCpp.h.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ryml.hpp>

#include "PALSParserCpp.h"
#include "pals_expression.h"
#include "pals_problem.h"
#include "pals_tree.h"

struct ParsedData;

namespace pals {

// ------------------------------------------------------------
// pals_combine.cpp: the `original` and `combined` trees
// ------------------------------------------------------------

// Collapse "." and "x/.." segments and repeated separators, lexically. Every
// file is keyed in `original` by its folded path.
std::string fold_path(const std::string& p);

// The `original` tree: every file the document is built from -- the top-level
// file and everything it reaches by `include` or `load`, at any depth -- keyed
// by its resolved path. `src` is the parsed top-level document (null if it
// could not be read or parsed) and is freed here; `filename` is the path it is
// keyed by. A top-level parse failure sets `parse_error` and returns an empty
// MAP.
YAMLTreeHandle make_original(ParsedData* src, const std::string& filename,
                             std::string& parse_error);

// The `combined` tree: the top-level file with every include spliced in and
// every `load`ed file merged, with provenance back into `original`.
YAMLTreeHandle make_combined_from_original(ParsedData* orig,
                                           const std::string& filename,
                                           ProblemList& problems);

// ------------------------------------------------------------
// pals_structure.cpp: structural expansion of the root lattice
// ------------------------------------------------------------

// The node an `inherit` names (lattice-element-parameters.md, s:inherit.params;
// lattice-elements.md): an element or a named parameter group (`ap1`), or a
// parameter group of an element (`q1>ApertureP`). ryml::NONE if not defined.
size_t inherit_source(const ryml::Tree& t,
                      const std::map<std::string, size_t>& emap,
                      const std::string& name);

// The lattice to expand: the one called `name` when it is given; otherwise the
// one the last `use` statement names; otherwise the last one in the file.
// ryml::NONE when there is none.
size_t find_lattice(const ryml::Tree& t, const std::string& name);

// Expand the lattice `lat_node` in place: bare names become the definitions
// they name, `repeat`s are unrolled, sub-lines spliced into the lines holding
// them, `inherit`s copied in and Forks given their branches. Then every branch
// is tidied (no `kind: BeamLine`, no Placeholders), checked for emptiness, and
// numbered for multipass. `emap` maps each definition name to its node.
void expand_lattice(ryml::Tree& t, size_t lat_node,
                    std::map<std::string, size_t>& emap, Provenance& prov,
                    ProblemList& problems);

// ------------------------------------------------------------
// pals_evaluate.cpp: expressions, constants/variables, controllers
// ------------------------------------------------------------

// What an expression is evaluated against: a resolver for user constants,
// variables and element-parameter references, and the species-name lookup that
// lets a particle-data function take a symbol.
struct EvalContext {
    SymbolLookup resolve;
    SpeciesLookup species;
};

// Build the evaluation context for `t`. Once the lattice `lat_node` is expanded
// an unqualified element name means an element of it, so its elements win over
// the facility definitions. The tables the context reads are held by
// shared_ptr, so it stays usable after this returns -- `set` commands need a
// context per command, since each one runs against a tree the previous ones
// have written to.
EvalContext make_expression_context(const ryml::Tree& t,
                                    size_t lat_node = ryml::NONE);

// A delayed (`expr(...)`) value substitute_values replaced with a number: the
// node, its key, the text it held, and the number written over it. Kept so the
// delayed expressions can be evaluated again once `post_expansion` has run
// (s:lattice.expand: "Evaluate the delayed expressions"). The key and number
// are what tell, later, that the node still holds this value and was neither
// removed nor written since.
struct DelayedValue {
    size_t node = ryml::NONE;
    std::string key;
    std::string text;     // the `expr(...)` source
    std::string written;  // the number substitute_values wrote
};

// How substitute_values treats a subtree.
struct SubstituteMode {
    // Evaluate only immediate expressions, leaving `expr(...)` values as text,
    // and keep quiet about what does not evaluate: the `facility` pass, which
    // evaluates what it can in list order and leaves the rest to the pass after
    // expansion.
    bool immediate_only = false;
    // Where to record the delayed values replaced, if anywhere.
    std::vector<DelayedValue>* delayed = nullptr;
};

// True if `node` is a map defining a `kind: Controller` element.
bool is_controller(const ryml::Tree& t, size_t node);

// Replace every evaluable scalar value in the subtree with its numeric value.
// Controller and `set` subtrees are skipped: their expressions are evaluated
// against a scope of their own, by the controller pass and execute_set. A value
// that looks like an expression but cannot be evaluated is recorded in
// `problems`.
void substitute_values(ryml::Tree& t, size_t node, const EvalContext& ctx,
                       ProblemList& problems,
                       const SubstituteMode& mode = SubstituteMode());

// The family `key` belongs to within `group` -- the parameters computed from
// one another, so that writing one leaves the rest stale -- including `key`
// itself; empty for a parameter that stands alone.
std::set<std::string> linked_family(const std::string& group,
                                    const std::string& key);

// Drop the rest of the family of the parameter `path` names on `ele`, so the
// bookkeeper derives them again from the value just written.
void nullify_family(ryml::Tree& t, size_t ele,
                    const std::vector<std::string>& path, Provenance& prov);

// The identifiers appearing in an expression: maximal runs of name characters
// that do not start with a digit.
std::vector<std::string> expression_identifiers(const std::string& s);

// Check the constants and variables (parameter-values-and-controllers.md,
// s:constants): where they are defined, that a name defined more than once is
// given the same value each time, and that no definition depends on itself.
void check_definitions(const ryml::Tree& t, ProblemList& problems);

// Evaluate all expressions in the (already expanded) tree in place, and apply
// the controllers to `lat_node`. `delayed`, when given, receives the delayed
// values replaced; `drivers`, the controllers driving each parameter.
void evaluate_expressions(
    ryml::Tree& t, size_t lat_node, Provenance& prov, ProblemList& problems,
    std::vector<DelayedValue>* delayed = nullptr,
    std::map<size_t, std::vector<size_t>>* drivers = nullptr);

// pals_controllers.cpp: evaluate the controllers, from the top of their
// hierarchy down, against the global symbols `global`, and apply the ABSOLUTE
// ones to the parameters they drive in the lattice `lat_node` (ryml::NONE
// evaluates without applying). Called by evaluate_expressions.
void evaluate_controllers(ryml::Tree& t, size_t lat_node,
                          const EvalContext& global, Provenance& prov,
                          ProblemList& problems,
                          std::vector<DelayedValue>* delayed,
                          std::map<size_t, std::vector<size_t>>* drivers);

// Put back the `expr(...)` text of the delayed values the first evaluation
// replaced, except those at `written` or no longer holding the number written
// over them. Returns the values put back.
std::vector<DelayedValue> restore_delayed(ryml::Tree& t,
                                          const std::vector<DelayedValue>& d,
                                          const std::set<size_t>& written);

// A delayed element parameter that came out different the second time is a new
// statement of its quantity: nullify the family members derived from the old.
void refresh_changed_delayed(ryml::Tree& t,
                             const std::vector<DelayedValue>& back,
                             Provenance& prov);

// ------------------------------------------------------------
// pals_sets.cpp: the `facility` and `post_expansion` lists
// ------------------------------------------------------------

// Everything a `set` needs to know about where it is writing: the element, the
// path to the parameter, the parameter key, and the group name for the family
// lookup.
struct SetTarget {
    size_t ele = ryml::NONE;
    std::vector<std::string> path;
    std::string group;  // "" for an ungrouped parameter such as `length`
    std::string key;
};

// Walk the `facility` list in order (s:lattice.expand), resolving each
// definition's inherits and immediate expressions as it is reached and running
// each `set` against the definitions before it.
void run_facility_list(ryml::Tree& t, const std::vector<size_t>& entries,
                       Provenance& prov, ProblemList& problems);

// Run the `set` commands of the `post_expansion` list against the expanded
// lattice. `written`, when given, collects one target per parameter written.
void run_post_expansion_sets(ryml::Tree& t, size_t lat_node,
                             const std::vector<size_t>& entries,
                             Provenance& prov, ProblemList& problems,
                             std::vector<SetTarget>* written = nullptr);

// ------------------------------------------------------------
// pals_bookkeeper.cpp: the per-branch walk
// ------------------------------------------------------------

// Walk every branch of the expanded lattice element by element, filling in the
// reference parameters, floor placement, s-position, element index and
// dependent parameters, capping each branch with a `branch_end`; then place the
// girders and every element's ShiftedFloorP.
void run_element_bookkeeper(ryml::Tree& t, size_t lat_node,
                            ProblemList& problems);

// Write `ForkP.forked_to` on every Fork and `ForkFromP` on its destination.
void link_fork_connections(ryml::Tree& t, size_t lat_node);

}  // namespace pals

#endif  // PALS_PIPELINE_H
