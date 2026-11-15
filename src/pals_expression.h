#ifndef PALS_EXPRESSION_H
#define PALS_EXPRESSION_H

/**
 * @file pals_expression.h
 * @brief Evaluator for PALS mathematical expressions (the "Mathematical
 *        Expressions", "Functions", and "Constants and Variables" sections of
 *        the PALS standard).
 *
 * Particle-data functions (mass_of, charge_of, anomalous_moment_of), which take
 * a quoted species name such as mass_of("#3He"), and the
 * named physical constants (c_light, r_electron, ...) are sourced from
 * AtomicAndPhysicalConstantsCLib so the whole PALS/Bmad ecosystem shares one
 * set of numbers.
 */

#include <functional>
#include <cstdint>
#include <string>

namespace pals {

// The `expr` group is defined in PALSParserCpp.h (@defgroup expr); the symbols
// below join it so the C entry point and this C++ interface document together.

/**
 * @brief Outcome of evaluating a PALS expression.
 * @ingroup expr
 *
 *   - `ok=true`                  -> `value` holds the finite numeric result.
 *   - `ok=false`                 -> not a (valid) evaluable expression: parse
 *     error, unknown identifier, unknown species, or a non-finite result. The
 *     caller should leave the original text untouched, and @ref error says
 *     which of those it was.
 */
struct EvalOutcome {
  bool ok = false;        ///< True when @ref value holds a finite result.
  double value = 0.0;     ///< The result; valid only when @ref ok is true.
  /**
   * Why evaluation failed, naming the offending symbol -- "unknown constant or
   * variable 'C_LIGHT'; did you mean 'c_light'?". A lower-case sentence
   * fragment, meant to be appended to a caller's own message. Empty when @ref
   * @ref ok.
   */
  std::string error;
};

/**
 * @brief Looks up a user-defined constant/variable by name.
 * @ingroup expr
 *
 * Returns true and sets `out` if the name is known. Built-in PALS constants
 * (pi, c_light, ...) are resolved inside the evaluator and need not be provided
 * here.
 */
using SymbolLookup = std::function<bool(const std::string& name, double& out)>;

/**
 * @brief Looks up a user-defined *species-name* symbol by name.
 * @ingroup expr
 *
 * A constant/variable whose value is a species string, e.g.
 * `species: "#3He"`. Returns true and sets `out` to the species name if the
 * name is known. Lets the particle-data functions accept a symbol, e.g.
 * `mass_of(species)`, not only a quoted literal `mass_of("#3He")`. An empty
 * std::function means no such symbols.
 */
using SpeciesLookup =
    std::function<bool(const std::string& name, std::string& out)>;

/**
 * @brief Reset the sequence random(), random_gauss() and random_gauss(sig_cut)
 * draw from (parameter-values-and-controllers.md, s:random.seed).
 * @ingroup expr
 *
 * The same nonzero seed gives the same draws from this parser. Zero asks for a
 * seed chosen by the system, so the draws differ from run to run. The sequence
 * is per thread.
 */
void seed_random(std::uint64_t seed);

/**
 * @brief Evaluates `text` as a PALS mathematical expression.
 * @ingroup expr
 *
 * A leading `expr(...)` wrapper, if present, must be stripped by the caller.
 * `lookup` resolves user symbols; an empty std::function is fine when none are
 * available. `species` resolves a bare identifier passed to a particle-data
 * function to a species name; an empty std::function restricts those functions
 * to quoted literals.
 */
EvalOutcome eval_expression(const std::string& text, const SymbolLookup& lookup,
                            const SpeciesLookup& species = {});

/**
 * @brief Resolves a built-in PALS constant by name.
 * @ingroup expr
 *
 * If `name` is a built-in PALS constant, sets `out` to its value and returns
 * true; otherwise returns false.
 */
bool builtin_constant(const std::string& name, double& out);

}  // namespace pals

#endif  // PALS_EXPRESSION_H
