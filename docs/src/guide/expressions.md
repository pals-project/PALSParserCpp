# Evaluating expressions

A PALS lattice may write numeric values as mathematical *expressions* —
`0.3 * r_electron`, `a_const^2`, `mass_of("proton")`. As part of expansion,
`parse_and_expand_PALS` evaluates every such expression to a plain number, so
both expanded trees are fully numeric. The `original` and `combined` views
always keep the expression text exactly as written.

The expression language itself — the grammar and its precedence, the built-in
constants, the functions, the syntax for defining constants and variables, for
`Controller` elements and for `set` commands — belongs to the PALS standard and
is documented with it at
[pals-project.readthedocs.io](https://pals-project.readthedocs.io). This page
covers what *this library* does with it: when evaluation happens, what it
declines to evaluate, and what it reports rather than guesses.

Constant values come from
[AtomicAndPhysicalConstantsCLib](https://github.com/pals-project/AtomicAndPhysicalConstantsCLib)
(APC — a C++ mirror of
[AtomicAndPhysicalConstants.jl](https://github.com/bmad-sim/AtomicAndPhysicalConstants.jl),
CODATA 2022), fetched automatically by CMake, so PALSParserCpp shares one set of
numbers with the rest of the toolchain.

## Where the standard leaves a choice

The standard fixes the operator precedence (`^` right-associative, a unary
sign looser than `^`, so `-2^2` is `-4` and `2^-1` is `0.5`). One point it does
not settle, which this library therefore has to fix:

- **Species names must be quoted.** `mass_of("proton")` is read; an unquoted
  name is an error. A mass number carries a leading `#` — `"#3He"`, not `"3He"`
  — matching
  [AtomicAndPhysicalConstants.jl](https://github.com/bmad-sim/AtomicAndPhysicalConstants.jl),
  and quoting is also what lets that `#` be written without tripping YAML's
  comment rule. The argument may instead be a constant or variable whose value
  is a species name, passed unquoted; such a definition may also be used
  directly wherever a species name is expected, and a bare identifier used as a
  parameter value is replaced by the species string it stands for.

## What gets evaluated

Only scalars that are genuine expressions are touched. Names that happen to sit
where a value could go — element and line references in a `line:`, `kind:`
names, booleans, a `MetaP` description — are not expressions and are left
alone. Immediate expressions (`length: 0.3 * r_electron`) and delayed ones
(`Kn1: expr(3.74 * a_var)`) both become numbers, but at different times:

- An **immediate** expression is evaluated once, when its definition is reached
  in the `facility` list. A `set` further down the list does not change it,
  even when it writes a parameter the expression uses. What it cannot evaluate
  yet — a parameter only expansion computes — is evaluated after expansion.
- A **delayed** expression is evaluated after expansion and again after the
  `post_expansion` list, so it follows a `post_expansion` set on a parameter it
  uses. A `set` whose value is delayed stores the expression rather than its
  value. Control expressions are delayed by definition.

The `expr(...)` wrapper must enclose the whole expression; `2 * expr(a)` is
reported. A constant may not be delayed, and a controller variable's initial
value may not be either.

Inheritance follows the same order. An `inherit` (of an element, a group, or
another element's group, `q1>ApertureP`) copies the values the source has when
the inheriting definition is reached, whether that definition stands under
`facility` or in place in a line (`- q2: {inherit: q1}`). A later `set` on the
source does not touch it. A line item that only uses an element (`- q1`, or
`- q1: {length: 0.3}` with values of its own) gets the element's parameters at
branch expansion, so it does follow such a set, except where it sets a value
itself.

A value that fails to evaluate is left as text rather than aborting the
expansion, and it is reported as a problem *only if it looks like one* — an
operator, a parenthesis, a `>` reference, or an explicit `expr(...)`. That is
what keeps a plain name or a label from being mistaken for broken arithmetic.

Some keys are excluded from evaluation outright, because their values are names
or free-form prose that could otherwise collide with a constant or parse as
arithmetic — `kind`, `use` and `inherit`, the fork targets, and the `authors`,
`notes` and `reminders` prose among them.

Constants and variables are resolved in dependency order, so a value may
reference one written later in the file as readily as one written earlier. A
reference that cannot be resolved, or a genuine cycle, leaves the value as text
and is reported. Nothing in `facility` may use a constant, variable or
controller defined in `post_expansion`.

### Randomness is deferred, never invented

An expression that calls `random()` or `random_gauss()` is left as text, so that
expanding the same lattice twice gives byte-identical output:

```yaml
Kn2: 0.01 + 0.003*random_gauss()   # kept verbatim
```

The same rule reaches everywhere such an expression can appear: a control
expression containing one is deferred and so drives nothing, and a `set` whose
`value` calls one leaves its parameter untouched.

`absolute_error` and `relative_error` follow from the same principle. The
standard gives the error *magnitude* — `absolute_error + relative_error *
|value|` — but not its distribution, so this library reads them, writes the
deterministic value, and reports a problem rather than picking a distribution
on the author's behalf.

## How sets are applied

A parameter of a known element that has not been written reads as **zero**, so
`PARAMETER + 0.02` works on an element that carries no such group yet — the
group is created. The exception is a parameter that is *still to be derived*: if
any member of its linked family has been written (the four interchangeable forms
`BnN`/`BnNL`/`KnN`/`KnNL` of a magnetic multipole component, the two forms of an
electric one, or the tied `BendP` geometry), its value is the bookkeeper's to
compute and reading it beforehand is an error:

```yaml
- Q1:
    kind: Quadrupole
    MagneticMultipoleP:
      Ks1: 0.34
- set:
    parameter: Q2>MagneticMultipoleP.Bs1
    value: Q1>MagneticMultipoleP.Bs1   # error: Bs1 needs the reference momentum
```

`PARAMETER` reads an unwritten parameter as its default value. For most
parameters this is zero. Where the default is `null` (for example
`ApertureP.x_min`, `BendP.e1` or `RFP.frequency`), there is no value to read,
and using `PARAMETER` is an error.

This matters for a `BeamLine` line item that only uses an element, such as
`- q1`. Before branch expansion such an item has only the values written on
the item itself, so a `facility` set on `main>>q1>MagneticMultipoleP.Kn1L`
sees the default (zero), not the value in the `q1` definition. The value the
set writes on the item then overrides the definition's at expansion. To change
the values that line items get from the definition, put the set under
`post_expansion`.

### Interdependent parameters

The members of such a family state one physical quantity in different units, so
only one of them can be the statement of it. Writing one **nullifies the
previous settings of all the others**: they are removed, and the bookkeeper
derives them again from the value just written.

```yaml
- s1:
    kind: Sextupole
    length: 0.27
    MagneticMultipoleP:
      Ks2: 0.34
- set:
    parameter: s1>MagneticMultipoleP.Ks2L
    value: 0.5
```

Here `s1` states its skew sextupole component as `Ks2` and the set restates it
as `Ks2L`; the `Ks2` goes, and `Ks2` comes back as `0.5 / 0.27`. Without this
the two would be left contradicting each other and reported as inconsistent.

The rule holds for every writer that reaches a parameter from outside the
element definition — sets in `facility` and in `post_expansion`, and ABSOLUTE
controllers, which are ordered among themselves by the order they are evaluated
in. Two members written *inside* one element definition are a different matter:
neither is "previous", so that stays an inconsistency for the bookkeeper to
report.

### `facility`, `post_expansion` and where a set acts

Which list a set sits in decides what it writes:

- A set in `facility` acts on the element *definitions*, and only on those
  defined earlier in the list — an element defined after it is untouched. An
  unqualified name reaches the construct that defines the element, whether it
  stands under `facility` or is defined in place in a beamline's `line` (it has
  a `kind` or an `inherit` of its own). It does not reach a line item that only
  uses an element defined elsewhere; that answers to a name qualified by its
  beamline (`myline>>q1`). One definition is written once, so every use of it
  gets the same value.
- A set in `post_expansion` acts on the already-expanded lattice, so each copy
  is a separate element and is written separately. The bookkeeper has already
  run at this point, so a value may use computed parameters such as
  `SELF>s_position`.

## The order of the passes

```text
facility list, in order: inherits, immediate expressions, sets
  → branch and fork expansion
  → expression evaluation (the rest, delayed ones included)
  → ABSOLUTE controllers
  → element bookkeeper
  → post_expansion sets
  → delayed expressions evaluated again
  → ABSOLUTE controllers again
  → element bookkeeper again
```

Applying the controllers last is what keeps a controller authoritative over a
parameter a later set also writes. The second bookkeeper pass recomputes what
those writes invalidated: the reference, floor and position values, and the
family members a `post_expansion` set or a controller nullified.

The snapshot that separates `expanded` from `full_expanded` is taken between the
first expression evaluation and the first bookkeeper pass — see
[The five trees](trees.md).

## Evaluating a single expression

`evaluate_pals_expression` evaluates one expression string on its own and
returns a `double`, setting an optional `ok` flag:

```cpp
bool ok = false;
double v = evaluate_pals_expression("3.75e7 / c_light^2", &ok);
// ok == true, v == 4.172…e-10
```

This evaluates a *standalone* string, so user-defined constants and variables
are **not** in scope — use `parse_and_expand_PALS` for whole-lattice evaluation,
whose expanded trees already have every expression resolved. `ok` is set to
`false` on a parse error, an unknown identifier or species, an unquoted species
name, a `random()`/`random_gauss()` expression (intentionally deferred), or a
non-finite result.
