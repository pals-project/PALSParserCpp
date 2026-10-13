# Building and using the library

## Building

From the repository root:

```console
cmake -S . -B build
cmake --build build
```

This builds `libPALSParserCpp` (`.dylib`/`.so`), a shared library that other
languages can load. CMake fetches the dependencies —
[rapidyaml](https://github.com/biojppm/rapidyaml),
[PCRE2](https://www.pcre.org), and
[AtomicAndPhysicalConstantsCLib](https://github.com/pals-project/AtomicAndPhysicalConstantsCLib) —
automatically. Rebuild after changing any source file, and before running the
tests. Lattices for the examples go under `lattice_files/`; the test fixtures are
separate, under `tests/lattices/`.

## Expanding a lattice

`parse_and_expand_PALS` reads a lattice file, resolves its includes and loads,
expands the selected lattice, and returns a `lattices` struct with five
independent views —
`original`, `combined`, `expanded`, `full_expanded`, and `adjunct` (see
[The five trees](trees.md)). Each is a `YAMLTreeHandle` and must be freed with
`delete_tree`.

```cpp
#include "PALSParserCpp.h"

struct lattices lat = parse_and_expand_PALS("ex.pals.yaml", nullptr);

char* s = tree_to_string(lat.full_expanded);
std::puts(s);
yaml_free_string(s);

// Report any problems met while expanding (see below), then free everything.
for (size_t i = 0; i < lat.problems.count; ++i)
    std::fprintf(stderr, "  - %s\n", lat.problems.items[i].message);
free_lattice_problems(lat.problems);

delete_tree(lat.original);
delete_tree(lat.combined);
delete_tree(lat.expanded);
delete_tree(lat.full_expanded);
delete_tree(lat.adjunct);
```

The second argument names the lattice to expand. Pass `nullptr` (or an empty
string) to expand the lattice named by the last `use` statement, or — if there
is none — the last lattice defined in the file.

A document already in memory — one a program generated, or has the text of — is
expanded by `expand_PALS_string`, which is `parse_and_expand_PALS` in every
respect but where the document comes from, so nothing need be written to disk to
be expanded:

```cpp
struct lattices lat = expand_PALS_string(doc, nullptr);
```

A string has no directory of its own, so a relative `include` or `load` inside
one resolves against the current working directory, and the document is keyed in
`original` as `<string>` rather than by a path. Read a document that names other
files with `parse_and_expand_PALS`, which resolves each reference against the
file that made it.

### Problems found during expansion

Expansion does not abort on a recoverable problem. It leaves the offending
value as it found it, carries on with the rest of the lattice, and appends an
entry to `lat.problems`, an owning `problem_list`. The list is empty when
expansion was clean. The library never prints — the caller decides whether to
report, save, or ignore what it finds — and must release the list with
`free_lattice_problems`.

Each entry carries more than its `message`. A `path` gives the logical spot it
was found at (`q1>ApertureP.shape`), empty when the problem is not tied to one
place. The other two answer questions a caller usually has to guess at:

- `severity` — `PROBLEM_ERROR` when the trees can no longer be trusted around
  the fault, `PROBLEM_WARNING` when expansion produced a sound result anyway.
- `origin` — `PROBLEM_INPUT` when the lattice author can fix it,
  `PROBLEM_UNSUPPORTED` when it is valid PALS this library does not implement
  yet, and `PROBLEM_UNSPECIFIED` when the standard does not define the case, so
  nothing was invented.

Editing the lattice can only ever clear a `PROBLEM_INPUT`, which is what makes
the distinction worth carrying: a tool that fails a build on any problem at all
will fail on lattices whose author has nothing left to fix.

Expansion is therefore always worth reading: a lattice with problems still
comes back expanded as far as it could be, with the trees around the fault
intact. The one exception is a top‑level file that is not valid YAML, where
there is nothing to expand: all five handles come back `NULL` and the parse
error, with its location, is the single problem reported.

What gets reported falls into a few groups.

**Structure.** A lattice named in the call (or in `use`) that does not exist, a
`line` reference to an undefined element or line, a missing
`inherit`/`repeat`/`Fork` target, an invalid `repeat` count, a `Fork` whose
`to_line` cannot be resolved, a branch that never got expanded, and a `load` or
`include` that cannot be read or that conflicts with what it is merged into.

**Expressions.** A value that cannot be evaluated — an unknown constant or
species, a dangling element‑parameter reference, a reference cycle, a syntax
error — is left as text and reported. Only values that *look* like expressions
(an operator, a parenthesis, a `>` reference, or an explicit `expr(...)`) are
flagged, so a plain name or a label is not mistaken for broken math. See
[Evaluating expressions](expressions.md).

**Controllers.** A `control_type` that is neither `ABSOLUTE` nor `RELATIVE`; a
`controls` entry with no `parameter` or no `expression`; a control target that
is malformed, names no element parameter, or matches nothing in the expanded
lattice; a reference to a variable a named controller does not have; a
controller variable whose initial value is not a constant expression; a
circular control hierarchy; a parameter driven by both an ABSOLUTE and a
RELATIVE controller; and a parameter that is both controlled and assigned a
delayed (`expr(...)`) expression.

**Sets.** A `set` with no `parameter` or no `value`; a target that is
malformed, names no element parameter, or matches nothing (for a
`facility` set, nothing *defined before it*); a value that cannot be
evaluated; and a value that reads a parameter whose value is still to be
derived during expansion.

**Placement.** A `CoordinateSetP` whose origin element matches no element, or
more than one, or one whose placement is not known first; two `Fiducial`s fixing
the same stretch of a branch; an element supported by two `Girder`s; a `Girder`
placed from `PREVIOUS_ELEMENT`; and a circular chain of girder support.

**Bookkeeping.** A branch whose first element gives neither a reference species
nor a reference energy, so the reference parameters cannot be computed; and a
parameter the author wrote that is inconsistent with what the rest of its
family or the bend geometry implies — the authored value is kept and the
disagreement reported rather than silently overwritten. (Two members of one
family stated in the *same* element definition are what this catches: a `set`
or a controller restating a family member nullifies the earlier statement
instead, so there is nothing to disagree with.)

**Vocabulary.** An element kind or parameter group name that is not one the
standard defines — a `FlorP` group is valid YAML and would otherwise go
unrecognised in silence — reported with a "did you mean" suggestion where a
near match exists. Also a name that breaks the name syntax or is a reserved word
(`SELF`, `PARAMETER`); a constant or variable redefined to a different value or
defined circularly; a `Girder` defined anywhere but under `PALS: girders`; a
`Lattice` or `use` under `post_expansion`; and an `adjustments` block that is not
a list of `optimize` blocks of `vary` and `target` entries.

**Deliberately not computed.** Where the standard fixes a value's magnitude but
not how to produce it, this library reports rather than guesses: an
`absolute_error`/`relative_error` on a `set` (the error distribution is
unspecified, so the deterministic value is written and the error is not), and a
`Foil` element's downstream species change (the stripping model is undefined,
so the species is left as the upstream one).

### Reading the expanded lattice

Which of the five trees answers a given question, and what the expansion adds to
`full_expanded` that appears nowhere else — `element_index`, `s_position`,
`ReferenceP`, `FloorP`, `ShiftedFloorP`, the derived parameter families, the
`branch_end` cap —
is covered in [The five trees](trees.md).

## Navigating and editing trees

A tree is addressed by an opaque `YAMLTreeHandle` plus integer `YAMLNodeId`s.
Walk it with `get_root`, `get_child_by_key`, `get_child_by_index`, `get_parent`,
and `get_size`; inspect nodes with `is_map`, `is_sequence`, `is_scalar`,
`get_node_key`, and `as_string`; and edit with `add_scalar`, `add_map`,
`add_sequence`, `set_scalar`, `set_node_key`, `remove_node`, `deep_copy_node`,
and `deep_copy_children`. Serialize with `node_to_string` / `tree_to_string`, or
`write_file`. Every one of these is listed in the [API Reference](../api.md).

Three more entry points build on the expanded tree:

- `build_correspondence_map` links a node across the derivation chain — `original`,
  `combined`, `full_expanded` and `adjunct` (their provenance is recorded as the
  trees are derived from one another). `expanded` takes no part: it is a pruned
  copy of `full_expanded`, so its nodes are found by path.
- `match_names` finds every named construct that a PALS *Name Matching* string
  refers to.
- `get_parameter_value` looks up a single parameter value by a *Name Matching*
  string — an element parameter (with a path) or, as a bare name, a constant or
  variable. It returns a `param_value` tagged as a number, a string, or missing.
  The value is returned as stored, **not** evaluated: a plain number comes back as
  `PARAM_VALUE_NUMBER`, while an expression (e.g. `0.3 * 5`), a species name such
  as `#3He`, or other text comes back verbatim as a string (evaluating is the job
  of expansion — the expanded trees already hold numbers). An unset element
  parameter yields its default (`0` for now); and a string that identifies no
  single value — no matching element/constant/variable, a bare element name, a
  path stopping on a whole group, or matches that disagree — yields
  `PARAM_VALUE_MISSING`. When the tag is `PARAM_VALUE_STRING`, free the returned
  `string` with `yaml_free_string`.

  ```c
  struct param_value v = get_parameter_value(full_expanded, "lat1>>>B1a>BendP.e1");
  if (v.kind == PARAM_VALUE_NUMBER) printf("%g\n", v.number);
  else if (v.kind == PARAM_VALUE_STRING) { puts(v.string); yaml_free_string(v.string); }
  ```
- `get_lattice_parameter_value` is the whole-lattice form: pass the `full_expanded`
  and `adjunct` handles from one `parse_and_expand_PALS()` result and it looks in
  `full_expanded` first (element parameters), then `adjunct` (constants, variables,
  and unused definitions) — never in the raw `original`/`combined` trees. Values
  therefore come back already evaluated. Pass `full_expanded` rather than
  `expanded`: a dependent parameter is a legitimate thing to ask for, and only the
  former carries one.

## Examples

The `examples/` directory contains runnable programs:

- **`examples/example_rw.cpp`** — read a lattice, do basic manipulations (add,
  remove, rename), print, and write it back out.

  ```console
  ./example_rw
  ```

- **`examples/print_lattices`** — expand a lattice and print all five views. It
  takes a file name and an optional `-lat <name>` flag naming the lattice to
  expand:

  ```console
  ./print_lattices ex.pals.yaml -lat lat2
  ```

  The file name is resolved under `../lattice_files/`, so run it from a
  directory that sits beside `lattice_files/` — `build/`, say — and put the
  lattice in `lattice_files/`.

## Extensions

[PALSParserJ](https://github.com/pals-project/PALSParserJ.jl) extends PALSParserCpp with a
Julia interface to the C API and translators from PALS to
[Bmad](https://github.com/bmad-sim/bmad-ecosystem) and
[SciBmad](https://github.com/bmad-sim/SciBmad.jl) lattice files.
