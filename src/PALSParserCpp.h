#ifndef PALS_PARSER_CPP_H
#define PALS_PARSER_CPP_H

/**
 * @file PALSParserCpp.h
 * @brief Public C API for parsing and manipulating PALS YAML lattices.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#define YAML_API __declspec(dllexport)
#else
#define YAML_API __attribute__((visibility("default")))
#endif

/**
 * @defgroup parse Parsing & expansion
 * @brief Read PALS/YAML from disk or memory and expand a lattice into its four
 *        representations.
 *
 * @defgroup correspond Node correspondence
 * @brief Map a single logical node across the original, combined, full_expanded
 *        adjunct trees.
 *
 * @defgroup namematch Name matching
 * @brief Resolve a PALS name-matching string to the nodes it selects.
 *
 * @defgroup param Parameter values
 * @brief Look up the stored value of a single element parameter, constant or
 *        variable.
 *
 * @defgroup expr Expression evaluation
 * @brief Evaluate a PALS mathematical expression to a number.
 *
 * @defgroup nav Navigation & inspection
 * @brief Walk a tree, read node keys and values, and test node types.
 *
 * @defgroup edit Building & editing
 * @brief Create trees and add, set, copy or remove nodes.
 *
 * @defgroup emit Serialization
 * @brief Emit a node or whole tree back to YAML text or a file.
 *
 * @defgroup lifecycle Memory management
 * @brief Release the handles and strings the API returns.
 */

// --- CORE TYPES ---

/** Opaque handle to a parsed YAML tree. */
typedef void* YAMLTreeHandle;
/** Identifies a node within a tree. */
typedef size_t YAMLNodeId;

/** Sentinel node id meaning "not found" or "invalid" (ryml's `(size_t)-1`). */
#define YAML_NULL_ID ((size_t)-1)

/**
 * Pass as the index argument to the add_* functions to append instead of
 * inserting.
 *
 * Spelled YAML_END rather than END because this is a public header: an
 * unprefixed END would clobber any enumerator, variable or macro of that name
 * in every translation unit that includes us.
 */
#define YAML_END YAML_NULL_ID

/**
 * @brief Whether a problem stops the expanded trees from being trustworthy.
 * @ingroup parse
 */
enum problem_severity {
    /**
     * The document is wrong here and the expansion could not work around it: a
     * value is missing, unresolved, or left at a stand-in. Do not trust the
     * affected part of `expanded` / `full_expanded`.
     */
    PROBLEM_ERROR = 0,
    /**
     * Expansion produced a usable result. Something was assumed, skipped, or
     * merely looks wrong -- worth reporting, but the trees are still sound.
     */
    PROBLEM_WARNING = 1
};

/**
 * @brief Who has to act on a problem.
 * @ingroup parse
 *
 * Separated from @ref problem_severity because the two answer different
 * questions: severity says whether the output can be trusted, origin says
 * whether the lattice author can do anything about it. A caller writing a
 * lattice editor wants to underline @ref PROBLEM_INPUT and stay quiet about the
 * rest; a caller filing bug reports wants the opposite.
 */
enum problem_origin {
    /** The document is wrong. The author can fix it. */
    PROBLEM_INPUT = 0,
    /** Valid PALS that this library does not implement yet. Not the author's
     *  fault, and not fixable by editing the lattice. */
    PROBLEM_UNSUPPORTED = 1,
    /** The PALS standard does not define this case, so nothing was invented.
     *  Neither the author nor this library is in the wrong. */
    PROBLEM_UNSPECIFIED = 2
};

/**
 * @brief One problem found while reading or expanding a document.
 * @ingroup parse
 */
struct problem {
    char* message;  ///< Human-readable description, always present (owning).
    /**
     * Logical location within the document -- typically `group.parameter` or an
     * element name -- or the empty string when the problem is not tied to one
     * spot. Deliberately shallow, so a definition reached through several
     * expansion copies reports one location rather than one per copy. This is
     * not a file name or a line number; @ref message already names the file
     * where the file is the point.
     */
    char* path;
    enum problem_severity severity;  ///< Is the output still trustworthy?
    enum problem_origin origin;      ///< Whose problem is it?
};

/**
 * @brief A flat, owning list of problems.
 * @ingroup parse
 *
 * Used to return everything encountered while building the `full_expanded` tree
 * (undefined lattice, dangling element/line references, undefined
 * `inherit`/`repeat`/`Fork` targets, expressions that could not be evaluated,
 * ...). Free with free_lattice_problems().
 */
struct problem_list {
    struct problem* items;  ///< The problems (owning).
    size_t count;           ///< Number of entries in @ref items.
};

/**
 * @brief The five trees parse_and_expand_PALS() produces, plus the problems
 *        found on the way.
 * @ingroup parse
 *
 * Plain C: the handles are opaque pointers, so this is usable from any language
 * that can read the rest of this header.
 */
struct lattices {
    YAMLTreeHandle original;  ///< Raw tree mapping each file the document is
                              ///< built from -- the top-level file and every
                              ///< file it reaches by "include" or "load" -- to
                              ///< its unparsed contents, keyed by path.
    YAMLTreeHandle combined;  ///< Tree with all "include" directives spliced
                              ///< inline and all "load"ed files merged in.
    YAMLTreeHandle expanded;  ///< @ref full_expanded minus every parameter the
                              ///< bookkeeper computed. Values are the finished
                              ///< ones: a parameter in both trees holds the
                              ///< same value in both.
    YAMLTreeHandle full_expanded;  ///< The expanded root lattice, and nothing
                                   ///< else: a map holding the single `name:
                                   ///< {kind: Lattice, ...}` entry, without the
                                   ///< PALS/facility scaffolding it was defined
                                   ///< under. Every dependent parameter is
                                   ///< computed and present.
    YAMLTreeHandle adjunct;  ///< Everything the expanded trees do not carry —
                              ///< the whole PALS/facility document minus the
                              ///< root lattice, so element/beamline
                              ///< definitions, `use` statements, constants and
                              ///< any non-root Lattice.
    struct problem_list problems;  ///< Problems found while building
                                   ///< `full_expanded`; free with
                                   ///< free_lattice_problems().
};

/**
 * @brief Links a node across the representations produced by
 *        parse_and_expand_PALS().
 * @ingroup correspond
 *
 * The trees are built as a derivation chain (original -> combined ->
 * full_expanded and adjunct), and provenance is recorded at every copy, so each
 * link records which node in each tree a single logical entity maps to.
 *
 * The mapping is functional per link: one full_expanded (or adjunct) node maps
 * to at most one combined node, which maps to at most one original node. Because
 * expansion can duplicate nodes (scalar substitution, `repeat`, `inherit`,
 * forks), a single combined/original node may appear in several links — one per
 * copy. A field is YAML_NULL_ID when no corresponding node exists (e.g. the
 * `destination_pointer` scalars synthesised during expansion have no original
 * source).
 *
 * Expansion splits the document, so a link carries either a `full_expanded` id
 * or a `adjunct` id, never both; the two are tied together through the
 * `combined` id they share. A definition that was substituted into the lattice
 * therefore yields links on both sides: one for the copy in `full_expanded`, one
 * for the definition still standing in `adjunct`.
 *
 * The `expanded` tree is not mapped: it is a pruned copy of `full_expanded`, so
 * a node in it is found by the path it sits at, not by a recorded link.
 */
struct node_link {
    YAMLNodeId original;  ///< Node in the `original` tree, or YAML_NULL_ID.
    YAMLNodeId combined;  ///< Node in the `combined` tree, or YAML_NULL_ID.
    YAMLNodeId full_expanded;  ///< Node in the `full_expanded` tree, or
                               ///< YAML_NULL_ID.
    YAMLNodeId adjunct;  ///< Node in the `adjunct` tree, or YAML_NULL_ID.
};

/**
 * @brief A flat list of node_links.
 * @ingroup correspond
 *
 * One link is emitted per node of the full_expanded tree and one per node of the
 * adjunct tree. Free with free_correspondence_map().
 */
struct correspondence_map {
    struct node_link* links;  ///< The links (owning).
    size_t count;             ///< Number of links in @ref links.
};

/**
 * @brief A flat list of node ids identifying every named construct that matched
 *        a query string.
 * @ingroup namematch
 *
 * Each id is a node within the single tree that was passed to match_names().
 * Free with free_name_matches().
 */
struct name_matches {
    YAMLNodeId* nodes;  ///< The matched node ids (owning).
    size_t count;       ///< Number of ids in @ref nodes.
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Builds and returns all four representations of a lattice file.
 * @ingroup parse
 *
 * @param filename     Path to the top-level YAML lattice file.
 * @param root_lattice Name of the lattice to expand. If NULL or empty:
 *                       - expands the lattice named by the last "use"
 * statement, or
 *                       - expands the last lattice defined in the file if no
 * "use" is present.
 * @return A `lattices` struct containing five handles:
 *           - `original`: raw tree mapping each file (including includes) to
 * its unparsed contents.
 *           - `combined`: tree with all "include" directives resolved and
 * spliced inline.
 *           - `full_expanded`: the selected lattice fully expanded — scalars
 * substituted, repeats unrolled, inherits merged, and forks resolved — and
 * nothing else. Rooted at a map holding the single `name: {kind: Lattice, ...}`
 * entry, stripped of the PALS/facility scaffolding it was defined under. Its
 * `branches` entries are branches, not the BeamLines they were built from, and
 * so carry no `kind`; a BeamLine referenced inside a `line:` is a sub-line whose
 * contents are spliced directly into the enclosing line, so no nested BeamLine
 * survives. Elements of a `multipass` line carry a `multipass_index` giving
 * their pass number — how many times a particle will have travelled through
 * that physical element by that point. Every dependent parameter has been
 * computed: each element carries its `element_index` — its position, counting
 * from one, in the branch line that holds it — its `ReferenceP`, `FloorP`,
 * `ShiftedFloorP` and `s_position`,
 * the derived members of every parameter family it uses (`Kn1L` alongside
 * `Kn1`, `voltage` alongside `gradient`, ...), the non-zero defaults of the
 * groups it carries, and each branch is capped with a `branch_end` Placeholder
 * holding its final reference and floor.
 *           - `expanded`: the same lattice with all of that removed. Which
 * parameters it keeps is decided by what the author wrote: one is held when it
 * was present before bookkeeping ran or was written by a `post_expansion`
 * `set`, and everything else the bookkeeper computed is pruned, along with any
 * group left empty and the `branch_end` elements. The *values* it holds are the
 * finished ones — this is `full_expanded` with nodes removed, not an earlier
 * snapshot, so a parameter present in both trees carries the same value in
 * both, with every `set` and ABSOLUTE controller applied. Use it to see what
 * was asked for rather than what it implies, or to write a lattice back out
 * without the derived values.
 *           - `adjunct`: the rest of the document, keeping its PALS/facility
 * scaffolding: element and beamline definitions, `use` statements, constants,
 * `post_expansion`, `adjustments`, the girders (with the `FloorP` and
 * `ShiftedFloorP` computed for them), and any Lattice that was not the one
 * expanded. Definitions substituted into
 * the lattice are copies, so they appear in both trees.
 *         All five handles must be freed individually with delete_tree().
 *
 *         The returned struct also carries `problems`: an owning list of every
 *         issue met while building the `full_expanded` tree (undefined lattice,
 *         dangling element/line references, undefined `inherit`/`repeat`/`Fork`
 *         targets, and expressions that could not be evaluated). Each carries a
 *         message, a shallow logical path, a @ref problem_severity saying
 *         whether the trees can still be trusted, and a @ref problem_origin
 *         saying whether the lattice author can do anything about it. The list
 *         is empty when expansion was clean. The caller owns it and must
 *         release it with free_lattice_problems(). The library does not print —
 *         the caller decides whether to report.
 */
YAML_API struct lattices parse_and_expand_PALS(const char* filename,
                                      const char* root_lattice);

/**
 * Builds the same four representations from a document held in memory.
 * @ingroup parse
 *
 * Identical to parse_and_expand_PALS() in every respect but where the document
 * comes from: use this for a lattice a program has generated, or already has the
 * text of, so that it need not be written to disk to be expanded.
 *
 * A string has no directory of its own, so a relative `include` or `load` inside
 * one is resolved against the current working directory, and the document is
 * keyed in `original` as `<string>` rather than by a path. A document that names
 * other files is better read with parse_and_expand_PALS(), which resolves each
 * reference against the file that made it.
 *
 * @param yaml_str     The document text. NULL is a parse failure, reported the
 *                     way an unreadable file is.
 * @param root_lattice Name of the lattice to expand; see parse_and_expand_PALS()
 *                     for what NULL or empty selects.
 * @return The same five handles and `problems` list as parse_and_expand_PALS(),
 *         owned by the caller on the same terms.
 */
YAML_API struct lattices expand_PALS_string(const char* yaml_str,
                                            const char* root_lattice);

/**
 * Frees the problem array owned by the `problems` list of a `lattices` value,
 * along with the strings each entry holds.
 * @ingroup parse
 *
 * Passing a list with a NULL `items` pointer (e.g. when there were no problems)
 * is safe and has no effect.
 *
 * @param problems The `lattices::problems` list to free (passed by value).
 */
YAML_API void free_lattice_problems(struct problem_list problems);

/**
 * Evaluates a single PALS mathematical expression to a double.
 * @ingroup expr
 *
 * Supports the full PALS expression grammar: arithmetic (+ - * / ^), unary
 * signs, parentheses, the built-in constants (pi, c_light, r_electron, ...),
 * the math functions (sqrt, log, sin, floor, modulo, ...), and the
 * particle-data functions mass_of / charge_of / anomalous_moment_of (backed by
 * AtomicAndPhysicalConstantsCLib), whose species-name argument must be quoted,
 * e.g. mass_of("#3He"). A leading `expr(...)` wrapper is accepted and unwrapped.
 *
 * This entry point evaluates a standalone string: user-defined constants and
 * variables are NOT in scope (use parse_and_expand_PALS() for whole-lattice
 * evaluation, which resolves them). Expressions containing random() /
 * random_gauss() are treated as non-evaluable here.
 *
 * @param expr Null-terminated expression string.
 * @param ok   Optional out-param. Set to true on success, false on a parse
 *             error, unknown identifier/species, deferred random(), or a
 *             non-finite result. May be NULL.
 * @return The evaluated value on success, or 0.0 on failure.
 */
YAML_API double evaluate_pals_expression(const char* expr, bool* ok);

/**
 * Builds the node correspondence between the derivation-chain trees of a
 * `lattices` value.
 * @ingroup correspond
 *
 * Returns a flat list containing one `node_link` per node of the
 * `full_expanded` tree and one per node of the `adjunct` tree. Each link gives
 * the corresponding `combined` and `original` node ids (or YAML_NULL_ID where
 * none exists). Grouping the links by shared combined / original ids recovers,
 * for any node in any of those trees, the set of nodes it corresponds to in the
 * others.
 *
 * The `expanded` tree takes no part: it is a pruned copy of `full_expanded`
 * rather than a step in the derivation chain, and its nodes are located by path.
 *
 * The handles must come from the same parse_and_expand_PALS() call — the
 * provenance recorded during that call is what makes the mapping exact. The
 * `original` handle is accepted for API symmetry; the mapping is derived from
 * the provenance stored in `combined`, `full_expanded` and `adjunct`.
 *
 * @param original      Handle to the `original` tree.
 * @param combined      Handle to the `combined` tree.
 * @param full_expanded Handle to the `full_expanded` tree.
 * @param adjunct      Handle to the `adjunct` tree. May be NULL, in which case
 *                      only the full_expanded tree is walked.
 * @return A correspondence_map. The caller must free it with
 *         free_correspondence_map(). `links` is NULL and `count` is 0 if
 *         `combined` is NULL, or if both `full_expanded` and `adjunct` are
 *         NULL.
 */
YAML_API struct correspondence_map build_correspondence_map(
    YAMLTreeHandle original, YAMLTreeHandle combined,
    YAMLTreeHandle full_expanded, YAMLTreeHandle adjunct);

/**
 * Frees the link array owned by a correspondence_map. Passing a map with a NULL
 * `links` pointer is safe and has no effect.
 * @ingroup correspond
 *
 * @param map The map to free (passed by value).
 */
YAML_API void free_correspondence_map(struct correspondence_map map);

/**
 * Finds every named construct matched by a PALS name-matching string.
 * @ingroup namematch
 *
 * The string follows the "Name Matching" syntax of the standard (names.md):
 *
 *   {element-set}[>{group}.{sub}. ... .{param}]
 *
 * where `{element-set}` is a `,` union of `&` intersections of `:` ranges of
 * element names, and an element name is
 *
 *   [{lattice}>>>][{branch}>>][{kind}::]{name}[#{N}]
 *
 * The operators bind, tightest first: `>>>`, `>>`, `::`, `#`, `:`, `&`, `,`,
 * `>`. `{lattice}`, `{branch}`, and `{name}` are PCRE2 patterns matched against
 * the whole name (anchored at both ends); characters inside a `(...)`, `[...]`
 * or `{...}` group belong to the pattern, so `Q\d{2,3}` is one name. A `{name}`
 * of digits only is the 1-based index of an element in its line. `{kind}` is an
 * element kind, `Controller`, or one of the MetaP components `alias`, `ID`,
 * `label` and `description` (`alias::black` matches the elements whose
 * `MetaP.alias` is `black`). `#{N}` keeps the N-th match. A range `{a}:{b}`
 * runs from each `a` to the next `b` in the same line, wrapping round the end
 * of the line. The parameter path after the single `>` is matched exactly, key
 * by key, and applies to every element of the set.
 *
 * In an expanded lattice (`expanded`, `full_expanded`) a name reaches the
 * elements of the lattice. In a tree that holds a `PALS` node (`original`,
 * `combined`, `adjunct`) an unqualified name reaches the element definitions
 * standing under `facility` or `post_expansion`, and only a `{branch}>>`
 * qualified name reaches the elements written in a beamline's `line` --
 * including a bare reference (`- q1`), and the elements of the sub-lines the
 * beamline uses.
 *
 * The node returned for each match is whatever the string resolves to: the
 * element node (no parameter path), or the parameter-group or parameter node
 * (with a path). Controllers are matched by `Controller::{name}`, and a
 * controller variable by `{controller}>{variable}`. A bare name -- one element
 * name with no qualifier of any sort and no parameter path -- additionally
 * matches each constant and variable of that name defined directly under the
 * `PALS`, `facility` or `post_expansion` node (both the full `kind: constant`/
 * `kind: variable` and the compact `constants:`/`variables:` forms), and each
 * controller of that name.
 *
 * Results are de-duplicated and returned in match order.
 *
 * @param tree         Handle to the tree to search.
 * @param match_string Null-terminated name-matching string.
 * @return A name_matches listing the matched node ids. `nodes` is NULL and
 *         `count` is 0 when there are no matches or on a malformed pattern. The
 *         caller must free it with free_name_matches().
 */
YAML_API struct name_matches match_names(YAMLTreeHandle tree,
                                         const char* match_string);

/**
 * Frees the node array owned by a name_matches. Passing a value with a NULL
 * `nodes` pointer is safe and has no effect.
 * @ingroup namematch
 *
 * @param matches The value to free (passed by value).
 */
YAML_API void free_name_matches(struct name_matches matches);

/**
 * @brief The three outcomes of get_parameter_value().
 * @ingroup param
 *
 * PARAM_VALUE_NUMBER also covers the "parameter identified but not set" case,
 * which returns the parameter's default value (0 for every parameter, for now —
 * real per-parameter defaults come later).
 */
enum param_value_kind {
    PARAM_VALUE_MISSING = 0,  ///< The match string did not identify a parameter.
    PARAM_VALUE_NUMBER = 1,   ///< Numeric value, in @ref param_value::number.
    PARAM_VALUE_STRING = 2,   ///< String value, in @ref param_value::string.
};

/**
 * @brief The result of get_parameter_value().
 * @ingroup param
 *
 * When `kind` is PARAM_VALUE_STRING, `string` is a heap-allocated
 * null-terminated copy the caller must release with yaml_free_string(); it is
 * NULL for the other two kinds, which need no freeing.
 */
struct param_value {
    int kind;       ///< One of enum param_value_kind.
    double number;  ///< Valid when kind == PARAM_VALUE_NUMBER.
    char* string;   ///< Valid when kind == PARAM_VALUE_STRING; NULL otherwise.
};

/**
 * Looks up the value of a single lattice parameter named by a PALS
 * name-matching string.
 * @ingroup param
 *
 * `match_string` uses the same syntax as match_names(). It names either an
 * element parameter (with a `>{group}.{sub}. ... .{param}` path) or, as a bare
 * name (no lattice/branch/kind qualifier and no path), a constant or variable —
 * the same constructs match_names() resolves. Run an element-parameter query on
 * the `full_expanded` tree of a parse_and_expand_PALS() result, where constants and
 * variables referenced by a value have already been substituted; constants and
 * variables themselves live in the facility scaffolding, so look them up in any
 * view that keeps it — `original`, `combined`, or `adjunct` — but not
 * the expanded trees, which drop it (exactly as with match_names()).
 *
 * The value is returned as stored; it is NOT evaluated. A plain numeric literal
 * comes back as a number (PARAM_VALUE_NUMBER); anything else — an expression such
 * as "0.3 * 5", a species name like "#3He", or any other text — comes back
 * verbatim as a string (PARAM_VALUE_STRING). Evaluation is the job of lattice
 * expansion (the expanded trees already hold numbers) or
 * evaluate_pals_expression, not of this accessor.
 *
 * Resolution, and the resulting `kind`:
 *   - Element parameter, set: its stored value — a number, or a string for an
 *     expression or other non-numeric text.
 *   - Element parameter, not set: the default value is returned
 *     (PARAM_VALUE_NUMBER with number 0, for now).
 *   - Constant or variable (bare name): its stored value, the same way.
 *   - Nothing is identified: PARAM_VALUE_MISSING. That covers no matching
 *     element, no matching constant/variable, a bare element name (an element
 *     has no single scalar value), a path that stops on a whole parameter group,
 *     and several matches that carry conflicting values. (Matches that agree —
 *     the same element reused, or several that all take the default — collapse
 *     to the one shared value.)
 *
 * @param tree         Handle to the tree to search.
 * @param match_string Null-terminated name-matching string.
 * @return A param_value. If `kind` is PARAM_VALUE_STRING the caller must free
 *         `string` with yaml_free_string().
 */
YAML_API struct param_value get_parameter_value(YAMLTreeHandle tree,
                                                const char* match_string);

/**
 * Looks up a parameter value across the two trees of an expanded lattice that
 * carry live values: the `full_expanded` lattice (element parameters, already
 * evaluated) and the `adjunct` facility scaffolding (constants, variables, and
 * any element/beamline definitions not spliced into the lattice).
 * @ingroup param
 *
 * This is the whole-lattice form of get_parameter_value(): the caller passes the
 * two relevant handles from a parse_and_expand_PALS() result instead of picking a
 * tree by hand. `full_expanded` is consulted first; only if it does not identify
 * the parameter is `adjunct` tried. The `original` and `combined` trees are
 * deliberately not consulted — they hold raw, pre-expansion text, so a value read
 * from them would be unevaluated and, for reused definitions, duplicated. Pass
 * `full_expanded` rather than `expanded`: a dependent parameter is a legitimate
 * thing to ask for, and only the former carries one.
 *
 * Either handle may be NULL (treated as "no match" for that tree). The value's
 * `kind`, string ownership, and the number/string/missing rules are exactly
 * those of get_parameter_value().
 *
 * @param full_expanded Handle to the `full_expanded` tree (element parameters).
 * @param adjunct      Handle to the `adjunct` tree (constants/variables).
 * @param match_string  Null-terminated name-matching string.
 * @return A param_value. If `kind` is PARAM_VALUE_STRING the caller must free
 *         `string` with yaml_free_string().
 */
YAML_API struct param_value get_lattice_parameter_value(
    YAMLTreeHandle full_expanded, YAMLTreeHandle adjunct,
    const char* match_string);

/**
 * Parses a YAML file from disk into an opaque tree handle.
 * @ingroup parse
 *
 * The file is read into an internal buffer and parsed in-place; the buffer
 * is owned by the returned handle and freed by delete_tree().
 *
 * @param filename Path to the YAML file to parse.
 * @return A tree handle on success, NULL if the file cannot be opened or
 *         if parsing fails.
 */
YAML_API YAMLTreeHandle parse_file(const char* filename);

/**
 * Parses a YAML string into an opaque tree handle.
 * @ingroup parse
 *
 * The string is copied into an internal buffer and parsed in-place; the
 * buffer is owned by the returned handle and freed by delete_tree().
 *
 * @param yaml_str Null-terminated YAML string to parse.
 * @return A tree handle on success, NULL if parsing fails.
 */
YAML_API YAMLTreeHandle parse_string(const char* yaml_str);

/**
 * Returns the most recent parse error message on the calling thread.
 * @ingroup parse
 *
 * Whenever parse_file(), parse_string() (and hence parse_and_expand_PALS())
 * returns NULL because a document is malformed or a file cannot be read, this
 * holds a human-readable description of why — for a YAML syntax error, prefixed
 * with the offending "line L, column C:" — so the caller can report exactly what
 * went wrong instead of a bare failure. It is cleared at the start of each parse
 * and is empty when the last parse on this thread succeeded.
 *
 * @return A null-terminated string owned by the library, valid until the next
 *         parse call on this thread. Never NULL (empty when there is no error).
 */
YAML_API const char* yaml_last_parse_error(void);

/**
 * Creates an empty tree with a MAP root node.
 * @ingroup edit
 *
 * Useful as a destination for deep_copy_node() / deep_copy_children(),
 * or for building a tree programmatically via add_map(), add_sequence(),
 * and add_scalar().
 *
 * @return A tree handle representing an empty MAP. Must be freed with
 * delete_tree().
 */
YAML_API YAMLTreeHandle create_empty_tree();

/**
 * Frees all memory associated with a tree handle.
 * @ingroup lifecycle
 *
 * @param tree Handle previously returned by parse_file(), parse_string(),
 *             create_empty_tree(), or parse_and_expand_PALS(). Passing NULL is safe
 *             and has no effect.
 */
YAML_API void delete_tree(YAMLTreeHandle tree);

/**
 * Removes a child node and all its descendants from the tree.
 * @ingroup edit
 *
 * After removal the child's node ID is invalid and must not be used again.
 * The parent parameter is accepted for API symmetry but is not used
 * internally — the parent is inferred from the tree structure.
 *
 * @param tree   Handle to the tree containing the node.
 * @param parent Node ID of the parent (unused, may be YAML_NULL_ID).
 * @param child  Node ID to remove. If YAML_NULL_ID, this call is a no-op.
 */
YAML_API void remove_node(YAMLTreeHandle tree, YAMLNodeId parent,
                          YAMLNodeId child);

/**
 * Returns the node ID of the tree's root node.
 * @ingroup nav
 *
 * @param tree Handle to a parsed or constructed tree.
 * @return Root node ID, or YAML_NULL_ID if tree is NULL.
 */
YAML_API YAMLNodeId get_root(YAMLTreeHandle tree);

/**
 * Returns the parent of a given node.
 * @ingroup nav
 *
 * @param tree Handle to the tree containing the node.
 * @param node Node ID whose parent is requested.
 * @return Parent node ID, or YAML_NULL_ID if node is the root or has no parent.
 */
YAML_API YAMLNodeId get_parent(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Finds a direct child of a MAP node by its key.
 * @ingroup nav
 *
 * @param tree   Handle to the tree.
 * @param parent Node ID of a MAP node to search.
 * @param key    Null-terminated key string to look up.
 * @return Node ID of the matching child, or YAML_NULL_ID if not found or
 *         if parent is not a MAP.
 */
YAML_API YAMLNodeId get_child_by_key(YAMLTreeHandle tree, YAMLNodeId parent,
                                     const char* key);

/**
 * Returns the nth child of a MAP or sequence node.
 * @ingroup nav
 *
 * @param tree   Handle to the tree.
 * @param parent Node ID of a MAP or sequence node.
 * @param index  Zero-based child index.
 * @return Node ID of the child at position index, or YAML_NULL_ID if index
 *         is out of range or parent is neither a MAP nor a sequence.
 */
YAML_API YAMLNodeId get_child_by_index(YAMLTreeHandle tree, YAMLNodeId parent,
                                       size_t index);

/**
 * Returns the number of direct children of a node.
 * @ingroup nav
 *
 * @param tree Handle to the tree.
 * @param node Node ID of a MAP or sequence node.
 * @return Number of children, or 0 if node is YAML_NULL_ID or is a scalar.
 */
YAML_API size_t get_size(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Returns the key of a node as a newly allocated null-terminated string.
 * @ingroup nav
 *
 * @param tree Handle to the tree.
 * @param node Node ID of a keyed node.
 * @return Heap-allocated string containing the key. The caller must free it
 *         with yaml_free_string(). Returns NULL if node is YAML_NULL_ID or
 *         the node has no key.
 */
YAML_API char* get_node_key(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Returns true if the node is a MAP (key-value container).
 * @ingroup nav
 *
 * @param tree Handle to the tree.
 * @param node Node ID to test.
 * @return true if the node is a MAP, false otherwise or if node is
 * YAML_NULL_ID.
 */
YAML_API bool is_map(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Returns true if the node is a sequence (ordered list).
 * @ingroup nav
 *
 * @param tree Handle to the tree.
 * @param node Node ID to test.
 * @return true if the node is a sequence, false otherwise or if node is
 * YAML_NULL_ID.
 */
YAML_API bool is_sequence(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Returns true if the node is a scalar (plain value, no children).
 * @ingroup nav
 *
 * @param tree Handle to the tree.
 * @param node Node ID to test.
 * @return true if the node is a scalar, false otherwise or if node is
 * YAML_NULL_ID.
 */
YAML_API bool is_scalar(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Returns the scalar value of a node as a newly allocated null-terminated
 * string.
 * @ingroup nav
 *
 * @param tree Handle to the tree.
 * @param node Node ID of a node that has a value.
 * @return Heap-allocated string containing the value. The caller must free it
 *         with yaml_free_string(). Returns NULL if node is YAML_NULL_ID or
 *         the node has no value (e.g. is a MAP or bare sequence).
 */
YAML_API char* as_string(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Adds a scalar key-value child to a MAP, or a plain scalar to a sequence.
 * @ingroup edit
 *
 * @param tree   Handle to the tree.
 * @param parent Node ID of the parent MAP or sequence.
 * @param key    Key string for MAP parents. Pass NULL for sequence elements.
 * @param value  Null-terminated scalar value string.
 * @param index  Insertion position among existing children. Pass YAML_END
 * to append.
 * @return Node ID of the newly created child, or YAML_NULL_ID on failure.
 */
YAML_API YAMLNodeId add_scalar(YAMLTreeHandle tree, YAMLNodeId parent,
                               const char* key, const char* value,
                               size_t index);

/**
 * Adds a new empty MAP child to an existing MAP or sequence.
 * @ingroup edit
 *
 * @param tree   Handle to the tree.
 * @param parent Node ID of the parent MAP or sequence.
 * @param key    Key string when parent is a MAP. Pass NULL for sequence
 * elements.
 * @param index  Insertion position among existing children. Pass YAML_END
 * to append.
 * @return Node ID of the newly created MAP child, or YAML_NULL_ID on failure.
 */
YAML_API YAMLNodeId add_map(YAMLTreeHandle tree, YAMLNodeId parent,
                            const char* key, size_t index);

/**
 * Adds a new empty sequence child to an existing MAP or sequence.
 * @ingroup edit
 *
 * @param tree   Handle to the tree.
 * @param parent Node ID of the parent MAP or sequence.
 * @param key    Key string when parent is a MAP. Pass NULL for sequence
 * elements.
 * @param index  Insertion position among existing children. Pass YAML_END
 * to append.
 * @return Node ID of the newly created sequence child, or YAML_NULL_ID on
 * failure.
 */
YAML_API YAMLNodeId add_sequence(YAMLTreeHandle tree, YAMLNodeId parent,
                                 const char* key, size_t index);

/**
 * Sets or replaces the scalar value of an existing node.
 * @ingroup edit
 *
 * The value string is copied into the tree's internal arena.
 *
 * @param tree  Handle to the tree.
 * @param node  Node ID to update. If YAML_NULL_ID, this call is a no-op.
 * @param value Null-terminated value string to set.
 */
YAML_API void set_scalar(YAMLTreeHandle tree, YAMLNodeId node,
                         const char* value);

/**
 * Sets or replaces the key of an existing node.
 * @ingroup edit
 *
 * The key string is copied into the tree's internal arena.
 *
 * @param tree Handle to the tree.
 * @param node Node ID to update. If YAML_NULL_ID, this call is a no-op.
 * @param key  Null-terminated key string to set.
 */
YAML_API void set_node_key(YAMLTreeHandle tree, YAMLNodeId node,
                           const char* key);

/**
 * Deep-copies the contents of a source node into a destination node,
 * overwriting whatever was previously there. Works across different trees.
 * @ingroup edit
 *
 * Keys, values, type flags, and all descendants are copied. Strings are
 * duplicated into the destination tree's arena so the source tree may be
 * freed independently afterwards.
 *
 * @param dst_tree Handle to the destination tree.
 * @param dst_node Node ID in dst_tree to copy into.
 * @param src_tree Handle to the source tree (may be the same as dst_tree).
 * @param src_node Node ID in src_tree to copy from.
 *                 If either handle is NULL or either node is YAML_NULL_ID,
 *                 this call is a no-op.
 */
YAML_API void deep_copy_node(YAMLTreeHandle dst_tree, YAMLNodeId dst_node,
                             YAMLTreeHandle src_tree, YAMLNodeId src_node);

/**
 * Deep-copies the children of a source node into a destination node,
 * inserting them at the given position. Works across different trees.
 * @ingroup edit
 *
 * Only the children are copied — the source node itself is not. Existing
 * children of dst_node are preserved. Strings are duplicated into the
 * destination tree's arena.
 *
 * @param dst_tree Handle to the destination tree.
 * @param dst_node Node ID in dst_tree to copy children into.
 * @param src_tree Handle to the source tree (may be the same as dst_tree).
 * @param src_node Node ID in src_tree whose children are copied.
 * @param index    Insertion position among dst_node's existing children.
 *                 Pass YAML_END to append after all existing children, or 0 to
 *                 prepend before all existing children.
 *                 If either handle is NULL or either node is YAML_NULL_ID,
 *                 this call is a no-op.
 */
YAML_API void deep_copy_children(YAMLTreeHandle dst_tree, YAMLNodeId dst_node,
                                 YAMLTreeHandle src_tree, YAMLNodeId src_node,
                                 size_t index);

/**
 * Emits a node and its descendants as a YAML string.
 * @ingroup emit
 *
 * @param tree Handle to the tree.
 * @param node Node ID to emit. If YAML_NULL_ID or out of range, returns NULL.
 * @return Heap-allocated null-terminated YAML string. The caller must free it
 *         with yaml_free_string().
 */
YAML_API char* node_to_string(YAMLTreeHandle tree, YAMLNodeId node);

/**
 * Emits a tree as a YAML string. Same as `node_to_string` but defaulted to
 * the root.
 * @ingroup emit
 *
 * @param tree Handle to the tree.
 * @return Heap-allocated null-terminated YAML string. The caller must free it
 *         with yaml_free_string().
 */
YAML_API char* tree_to_string(YAMLTreeHandle tree);

/**
 * Writes the entire tree to a file as YAML.
 * @ingroup emit
 *
 * @param tree     Handle to the tree to serialize.
 * @param filename Path to the output file. Created or truncated if it already
 * exists.
 * @return true on success, false if the file could not be opened or if an
 *         error occurs during writing.
 */
YAML_API bool write_file(YAMLTreeHandle tree, const char* filename);

/**
 * Frees a string returned by get_node_key(), as_string(), node_to_string() or
 * tree_to_string(). These are every char*-returning function in this header.
 * @ingroup lifecycle
 *
 * Passing NULL is safe and has no effect.
 *
 * @param str Pointer to the string to free.
 */
YAML_API void yaml_free_string(char* str);

#ifdef __cplusplus
}
#endif

#endif  // PALS_PARSER_CPP_H
