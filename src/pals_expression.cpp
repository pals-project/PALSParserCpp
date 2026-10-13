/**
 * @file pals_expression.cpp
 * @brief Recursive-descent evaluator for PALS mathematical expressions.
 *
 * Grammar (`^` is right-associative):
 *   expr    := term (('+' | '-') term)*
 *   term    := unary (('*' | '/') unary)*
 *   unary   := ('+' | '-') unary | power
 *   power   := primary ('^' unary)?
 *   primary := number | name | name '(' args ')' | '(' expr ')'
 *
 * Note that `unary` sits above `power` rather than below it, so a leading sign
 * binds looser than `^` while the exponent may itself be signed: -2^2 == -4 and
 * 2^-1 == 0.5. This is the Fortran/Bmad convention the rest of the ecosystem
 * uses, and it differs from C-family languages where unary minus binds tighter.
 *
 * The particle functions mass_of / charge_of / anomalous_moment_of take a
 * quoted species name (e.g. `mass_of("#3He")`), not a numeric sub-expression, so
 * their argument is read as raw text and passed to
 * AtomicAndPhysicalConstantsCLib. An unquoted species name is an error.
 */

#include "pals_expression.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "apc/apc.h"

namespace pals {
namespace {

// std::numbers::pi is C++20; keep this a plain constant for C++17.
constexpr double kPi = 3.14159265358979323846;

// Thrown to abort a non-evaluable expression; caught at the top level, where
// `why` becomes EvalOutcome::error. Every throw site says what it saw, since
// "could not evaluate" on its own leaves the reader to find the bad symbol.
struct ParseError {
  std::string why;
};

std::string lowered(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

class Parser {
 public:
  Parser(const std::string& s, const SymbolLookup& lookup,
         const SpeciesLookup& species)
      : s_(s), lookup_(lookup), species_(species) {}

  double parse() {
    double v = expr();
    if (peek() != '\0')  // trailing junk
      throw ParseError{"unexpected '" + s_.substr(pos_) +
                       "' after the expression"};
    return v;
  }

  bool deferred() const { return deferred_; }

 private:
  const std::string& s_;
  const SymbolLookup& lookup_;
  const SpeciesLookup& species_;
  size_t pos_ = 0;
  bool deferred_ = false;

  void skip_ws() {
    while (pos_ < s_.size() &&
           std::isspace(static_cast<unsigned char>(s_[pos_])))
      ++pos_;
  }

  // Returns the next significant char (whitespace skipped) without consuming.
  char peek() {
    skip_ws();
    return pos_ < s_.size() ? s_[pos_] : '\0';
  }

  bool consume(char c) {
    if (peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  double expr() {
    double v = term();
    for (;;) {
      char c = peek();
      if (c == '+') {
        ++pos_;
        v += term();
      } else if (c == '-') {
        ++pos_;
        v -= term();
      } else {
        return v;
      }
    }
  }

  double term() {
    double v = unary();
    for (;;) {
      char c = peek();
      if (c == '*') {
        ++pos_;
        v *= unary();
      } else if (c == '/') {
        ++pos_;
        v /= unary();
      } else {
        return v;
      }
    }
  }

  // Unary signs bind looser than `^`, so -2^2 == -(2^2) and 2^-2 == 2^(-2),
  // matching the Fortran/Bmad convention used across the ecosystem.
  double unary() {
    char c = peek();
    if (c == '+') {
      ++pos_;
      return unary();
    }
    if (c == '-') {
      ++pos_;
      return -unary();
    }
    return power();
  }

  double power() {
    double base = primary();
    if (peek() == '^') {
      ++pos_;
      return std::pow(base, unary());  // right-associative; exponent may be signed
    }
    return base;
  }

  double primary() {
    char c = peek();
    if (c == '(') {
      ++pos_;
      double v = expr();
      if (!consume(')')) throw ParseError{"missing ')'"};
      return v;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') return number();
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return name();
    throw ParseError{c == '\0'
                         ? "expression ends where a number or name is expected"
                         : "expected a number or name, found '" +
                               std::string(1, c) + "'"};
  }

  double number() {
    skip_ws();
    const char* start = s_.c_str() + pos_;
    char* end = nullptr;
    double v = std::strtod(start, &end);
    if (end == start) throw ParseError{"expected a number"};
    pos_ += static_cast<size_t>(end - start);
    return v;
  }

  std::string read_identifier() {
    skip_ws();
    size_t start = pos_;
    bool dotted = false;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      // `>` joins a controller name to one of its variables (`ps27>cur1`) or an
      // element name to a parameter path (`thingB>MagneticMultipoleP.Kn2L`);
      // PALS has no `>` operator, so it is unambiguous inside an identifier.
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '>') {
        if (c == '>') dotted = true;
        ++pos_;
      } else if (c == '.' &&
                 (dotted || s_.compare(start, pos_ - start, "SELF") == 0)) {
        // A `.` is part of the identifier only in a parameter path: after the
        // `>` of an element-parameter reference, or after the `SELF` of a `set`
        // value expression (`SELF.BendP.g_ref`), where it separates the
        // parameter group from the parameter. Elsewhere `.` begins a number and
        // is not an identifier character.
        dotted = true;
        ++pos_;
      } else {
        break;
      }
    }
    return s_.substr(start, pos_ - start);
  }

  // Reads the raw, whitespace-trimmed argument text up to the matching ')'. The
  // opening '(' must already be consumed. No quote handling — the caller
  // decides whether it is a quoted literal or a symbol.
  std::string read_raw_arg() {
    int depth = 1;
    std::string out;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == '(') {
        ++depth;
      } else if (c == ')') {
        if (--depth == 0) {
          ++pos_;
          break;
        }
      }
      out.push_back(c);
      ++pos_;
    }
    if (depth != 0) throw ParseError{"missing ')'"};
    size_t a = out.find_first_not_of(" \t");
    size_t b = out.find_last_not_of(" \t");
    return (a == std::string::npos) ? "" : out.substr(a, b - a + 1);
  }

  // Enforces the leading-'#' rule for mass numbers: a mass number must be
  // written "#3He", not "3He" (matching AtomicAndPhysicalConstants.jl). The
  // mass number, when present, leads the name (after an optional "anti-"
  // prefix), so a leading ASCII digit signals a missing '#'.
  static void check_mass_number(const std::string& name) {
    std::string core = name;
    for (const char* pre : {"anti-", "Anti-", "anti", "Anti"}) {
      if (core.rfind(pre, 0) == 0) {
        core.erase(0, std::string(pre).size());
        break;
      }
    }
    if (!core.empty() && std::isdigit(static_cast<unsigned char>(core.front())))
      throw ParseError{"species '" + name +
                       "' needs a leading '#' on its mass number"};
  }

  // Reads the species-name argument of a particle-data function. It is either a
  // quoted literal (mass_of("#3He")) or a bare identifier that resolves through
  // the species symbol table to a species string (mass_of(species), where
  // `species: "#3He"`). The opening '(' must already be consumed.
  std::string read_species_arg() {
    std::string arg = read_raw_arg();
    std::string name;
    if (arg.size() >= 2 && (arg.front() == '"' || arg.front() == '\'') &&
        arg.back() == arg.front()) {
      name = arg.substr(1, arg.size() - 2);  // quoted literal
    } else if (!species_ || !species_(arg, name)) {
      // Not quoted and not a known species symbol: an unquoted literal name is
      // an error, and an unknown symbol cannot be resolved.
      throw ParseError{"'" + arg +
                       "' is neither a quoted species name nor a symbol naming "
                       "one"};
    }
    check_mass_number(name);
    return name;
  }

  double name() {
    std::string id = read_identifier();
    if (peek() == '(') {
      ++pos_;  // consume '('
      return call(id);
    }
    double v;
    if (builtin_constant(id, v)) return v;
    if (lookup_ && lookup_(id, v)) return v;
    // A `>` in the name makes it a reference to a controller variable or an
    // element parameter rather than a plain constant, so say which is missing.
    std::string what = id.find('>') == std::string::npos
                           ? "unknown constant or variable '"
                           : "unknown reference '";
    throw ParseError{what + id + "'" + case_hint(id)};
  }

  // "; did you mean 'c_light'?" when `id` differs from a name that does resolve
  // only in case. Every built-in constant is lower case, so lowering `id` and
  // retrying is the whole test -- no second table to drift out of step. The
  // same retry catches a user constant written `MY_CONST` but defined
  // `my_const`.
  std::string case_hint(const std::string& id) const {
    std::string low = lowered(id);
    if (low == id) return "";
    double v;
    if (builtin_constant(low, v) || (lookup_ && lookup_(low, v)))
      return "; did you mean '" + low + "'?";
    return "";
  }

  std::vector<double> arg_list() {
    std::vector<double> args;
    if (peek() == ')') {
      ++pos_;
      return args;
    }
    args.push_back(expr());
    while (peek() == ',') {
      ++pos_;
      args.push_back(expr());
    }
    if (!consume(')')) throw ParseError{"missing ')' after the arguments"};
    return args;
  }

  double call(const std::string& fn) {
    // Particle-data functions take a species name (quoted literal or a symbol
    // that resolves to one), not a numeric expression.
    if (fn == "mass_of" || fn == "charge_of" || fn == "anomalous_moment_of") {
      std::string sp = read_species_arg();
      try {
        if (fn == "mass_of") return apc::mass_of(sp);
        if (fn == "charge_of") return apc::charge_of(sp);
        return apc::anomalous_moment_of(sp);
      } catch (const std::exception&) {
        throw ParseError{"unknown species '" + sp + "'"};
      }
    }
    // random()/random_gauss() cannot be evaluated reproducibly: mark deferred
    // but still consume the argument list so parsing completes.
    if (fn == "random" || fn == "random_gauss") {
      arg_list();
      deferred_ = true;
      return 0.0;  // unused when deferred
    }
    return apply(fn, arg_list());
  }

  static double apply(const std::string& fn, const std::vector<double>& a) {
    auto unary = [&]() -> double {
      if (a.size() != 1)
        throw ParseError{"'" + fn + "' takes one argument, got " +
                         std::to_string(a.size())};
      return a[0];
    };
    auto binary = [&](int i) -> double {
      if (a.size() != 2)
        throw ParseError{"'" + fn + "' takes two arguments, got " +
                         std::to_string(a.size())};
      return a[i];
    };

    if (fn == "sqrt") return std::sqrt(unary());
    if (fn == "log") return std::log(unary());
    if (fn == "exp") return std::exp(unary());
    if (fn == "sin") return std::sin(unary());
    if (fn == "cos") return std::cos(unary());
    if (fn == "tan") return std::tan(unary());
    if (fn == "cot") { double x = unary(); return std::cos(x) / std::sin(x); }
    if (fn == "sinc") { double x = unary(); return x == 0.0 ? 1.0 : std::sin(x) / x; }
    if (fn == "asin") return std::asin(unary());
    if (fn == "acos") return std::acos(unary());
    if (fn == "atan") return std::atan(unary());
    if (fn == "atan2") return std::atan2(binary(0), binary(1));
    if (fn == "sinh") return std::sinh(unary());
    if (fn == "cosh") return std::cosh(unary());
    if (fn == "tanh") return std::tanh(unary());
    if (fn == "coth") { double x = unary(); return std::cosh(x) / std::sinh(x); }
    if (fn == "asinh") return std::asinh(unary());
    if (fn == "acosh") return std::acosh(unary());
    if (fn == "atanh") return std::atanh(unary());
    if (fn == "acoth") return std::atanh(1.0 / unary());
    if (fn == "abs") return std::fabs(unary());
    if (fn == "factorial") return std::tgamma(unary() + 1.0);
    if (fn == "int") return std::trunc(unary());       // toward zero
    if (fn == "nint") return std::round(unary());      // nearest, half away
    if (fn == "sign") { double x = unary(); return (x > 0) - (x < 0); }
    if (fn == "floor") return std::floor(unary());
    if (fn == "ceiling") return std::ceil(unary());
    if (fn == "modulo") {
      double x = binary(0), p = binary(1);
      return x - std::floor(x / p) * p;
    }
    // Whether the lower-case spelling is a function is settled by trying it:
    // the names live in the chain above and nowhere else, so probing keeps the
    // hint from going stale. The result is thrown away -- a miscased name is
    // still an error, not a silent correction -- and the recursion stops
    // because a lowered name lowers to itself.
    std::string hint;
    std::string low = lowered(fn);
    if (low != fn) {
      try {
        apply(low, a);
        hint = "; did you mean '" + low + "'?";
      } catch (const ParseError&) {
      }
    }
    throw ParseError{"unknown function '" + fn + "'" + hint};
  }
};

}  // namespace

bool builtin_constant(const std::string& name, double& out) {
  // Physical values come from AtomicAndPhysicalConstantsCLib (APC, CODATA
  // 2022) so PALS shares the ecosystem's numbers; pi is exact.
  //
  // classical_radius_factor is the one deliberate exception: apc::
  // CLASSICAL_RADIUS_FACTOR is r_e*m_e with the mass in MeV, while APC's own
  // M_ELECTRON is in eV, so the exported constant is 1e6 larger than the value
  // PALS wants. Deriving it here from APC's own r_electron and M_ELECTRON keeps
  // it in eV*m and matches Bmad's e^2/(4*pi*eps_0) = 1.44e-9 eV*m. Do not
  // "simplify" this to apc::CLASSICAL_RADIUS_FACTOR without resolving the units
  // question upstream first — it would silently introduce a factor of 1e6.
  if (name == "pi") { out = kPi; return true; }
  if (name == "twopi") { out = 2 * kPi; return true; }
  if (name == "c_light") { out = apc::C_LIGHT; return true; }
  if (name == "h_planck") { out = apc::H_PLANCK; return true; }
  if (name == "hbar") { out = apc::H_BAR; return true; }
  if (name == "k_boltzmann") { out = apc::K_BOLTZMANN; return true; }  // eV/K
  if (name == "r_electron") { out = apc::R_ELECTRON; return true; }
  if (name == "r_proton") { out = apc::R_PROTON; return true; }
  if (name == "e_charge") { out = apc::E_CHARGE; return true; }
  if (name == "mu_0") { out = apc::MU_0; return true; }
  if (name == "epsilon_0") { out = apc::EPS_0; return true; }
  if (name == "classical_radius_factor") {
    out = apc::R_ELECTRON * apc::M_ELECTRON;
    return true;
  }
  if (name == "fine_structure") { out = apc::FINE_STRUCTURE; return true; }
  if (name == "n_avogadro") { out = apc::AVOGADRO; return true; }
  return false;
}

EvalOutcome eval_expression(const std::string& text, const SymbolLookup& lookup,
                            const SpeciesLookup& species) {
  EvalOutcome r;
  try {
    Parser p(text, lookup, species);
    double v = p.parse();
    if (p.deferred()) {
      r.deferred = true;
      return r;
    }
    if (!std::isfinite(v)) {  // ok stays false
      r.error = "the result is not a finite number";
      return r;
    }
    r.ok = true;
    r.value = v;
  } catch (const ParseError& e) {
    r.error = e.why;
  } catch (...) {
    // Not an evaluable expression; r.ok stays false.
    r.error = "not a valid expression";
  }
  return r;
}

}  // namespace pals
