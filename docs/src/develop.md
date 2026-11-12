# Working on PALSParserCpp

Notes for working on the library itself: where the code lives, how trees are
held in memory, how to run the tests, and how to build this documentation.

## Source layout

The library sources live in `src/`, split by concern:

- **`PALSParserCpp.h`** — the public C API: the opaque handle types and every
  exported function. This is the only header consumers include.
- **`PALSParserCpp.cpp`** — the generic YAML tree wrapper over rapidyaml
  (parse, traverse, query, modify, emit). Knows nothing about PALS.
- **`yaml_tree.h`** — internal declarations shared between the wrapper and the
  PALS code: the tree representation behind `YAMLTreeHandle` (`ParsedData`)
  plus the low-level tree helpers (`ensure_capacity`,
  `deep_copy_recursive`).
- **`pals_expand.cpp`** — the lattice expansion pipeline that builds the
  five-tree representation (`original` / `combined` / `expanded` /
  `full_expanded` / `adjunct`): include splicing, `load` merging, structural
  expansion (repeats, inherits, forks), expression, controller and `set`
  evaluation, and the element bookkeeper that walks each branch filling in
  reference parameters, floor placement, s-positions and dependent parameters.
- **`pals_check.{h,cpp}`** — spelling checks against the fixed PALS vocabulary
  (element kinds and parameter group names). A `FlorP` group parses as valid
  YAML and would otherwise go unrecognised in silence, so it is reported here,
  with a "did you mean" suggestion where a near match exists.
- **`pals_floor.{h,cpp}`** — floor (global) coordinate geometry. A placement is
  a position plus an orientation, carried as a unit quaternion rather than the
  standard's W matrix; `floor_propagate` advances one along the reference
  curve, and `straight_LS` / `bend_LS` / `patch_LS` build the (L, S) pair for
  the three geometries PALS defines.
- **`pals_match.cpp`** — PALS name matching and parameter lookup (the
  PCRE2-based `match_names` / `get_parameter_value` family).
- **`pals_util.{h,cpp}`** — small helpers shared across the PALS split
  (`child_val_str`, `split_dots`, `resolve_param_path`, `strip_expr_wrapper`).
- **`pals_expression.{h,cpp}`** — the standalone PALS expression grammar and
  evaluator (arithmetic, functions, built-in constants, particle-data lookups).

Everything builds into `libPALSParserCpp`; see `CMakeLists.txt`.

## Memory model

`YAMLTreeHandle` wraps a `ryml::Tree` into a C object so that trees can be
handed across the shared library boundary to other languages. A `ryml::Tree` is
stored in memory as a plain array of nodes, and a `ryml::NodeRef` is a thin
wrapper around an index into that array — which is what `YAMLNodeId` is.

Trees are obtained by parsing a C++ `std::string`, and the parsed values are
pointers into that string rather than copies. **The string must therefore stay
alive as long as the tree is in use**, which is why `ParsedData` owns both
together.

Most of the relevant ryml code for reference is in
`build/_deps/rapidyaml-src/src/c4/yml/tree.hpp`.

## Running the tests

The tests are [Catch2](https://github.com/catchorg/Catch2) cases under
`tests/`, registered with CTest. Rebuild before running them — the tests link
against the freshly built library:

```console
cmake --build build
ctest --test-dir build --output-on-failure
```

To run a single test case, match it by name:

```console
ctest --test-dir build -R "Test Name"
```

The test binary can also be run directly, from any directory:

```console
build/tests/tests "[check]"
```

### Where a test's lattice comes from

The tests do not create lattice files. A lattice a test needs only in order to
expand it is held in a string and passed to `expand_PALS_string`, which builds
the same four trees `parse_and_expand_PALS` does without going through the disk:

```cpp
struct lattices lat = expand_PALS_string(doc, nullptr);
```

The tests that are about files on disk — what a relative `load` or `include`
resolves against, what `parse_file` does with a real path, what a parse error
says a malformed file is called — read the fixtures committed under
`tests/lattices/`, one directory per multi-file case. `lattice_file("name")`
names one; it resolves against the test sources' own location, so the current
directory never matters. The two tests that exercise `write_file` write under the
system temp directory, through `out_path`.

Nothing about the tests depends on where they are started from. (The examples are
a different story: `example_rw` and `print_lattices` reach their lattices through
`../lattice_files/`, so they still have to be run from `build/`.)

### The standard's example lattices

A separate check runs every example lattice of the PALS standard
(`pals-project/pals/examples`) through the parser. Each `*.pals.yaml` file must
expand with no problems to a non-empty lattice, except those listed in
`tests/standard_examples_manifest.txt`, whose header documents the entry kinds.
The `standard_examples` workflow runs it in CI against the pals commit pinned in
`tests/standard_examples_git_reference.txt`, so a change to the standard cannot
break an unrelated PR. To check a newer pals commit, bump the pin by hand in a PR
of its own, along with any manifest or parser changes it needs.

To run it locally against a checkout of pals:

```console
cmake --build build --target validate_standard_examples
build/tests/validate_standard_examples --root <path-to-pals>/examples \
    --manifest tests/standard_examples_manifest.txt
```

## Building the documentation

The site is built with Sphinx (MyST Markdown + the Furo theme), with the C/C++
API pulled in from Doxygen via [Breathe](https://breathe.readthedocs.io). The
published site is at <https://pals-project.github.io/PALSParserCpp/>.

To build and preview it locally (requires `doxygen` and `python3`):

```console
docs/build_local.sh
```

This runs `docs/build.py` — Doxygen (API → XML), then Sphinx →
`docs/build/html` — and serves the result at <http://localhost:8000/>. Pass
`--no-serve` to build without starting a server, or `--port N` to serve
elsewhere.

Narrative pages live in `docs/src/`; the API reference is generated from the
doc comments in `src/PALSParserCpp.h` and `src/pals_expression.h`, so API
documentation is edited in the headers themselves.
