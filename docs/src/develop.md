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
- **The lattice expansion pipeline**, which builds the five-tree
  representation (`original` / `combined` / `expanded` / `full_expanded` /
  `adjunct`), one stage per file. **`pals_pipeline.h`** declares what each
  stage hands the next, and lists the order they run in:
  - **`pals_expand.cpp`** — the entry points (`parse_and_expand_PALS`,
    `expand_PALS_string`) and the correspondence-map C API. Runs the stages
    on a work tree, then cuts that tree into `expanded`, `full_expanded` and
    `adjunct`.
  - **`pals_combine.cpp`** — the `original` and `combined` trees: resolving
    file references, splicing `include`s and merging `load`ed files.
  - **`pals_structure.cpp`** — structural expansion of the root lattice:
    name substitution, `repeat`s, sub-lines, `inherit`s, forks, multipass.
  - **`pals_evaluate.cpp`** — expression evaluation over the tree, the
    constants and variables it reads, and the checks on them.
  - **`pals_controllers.cpp`** — controllers, evaluated down their hierarchy
    and applied to the parameters they drive.
  - **`pals_sets.cpp`** — the `facility` walk and `set` commands, before and
    after expansion.
  - **`pals_bookkeeper.cpp`** — the element bookkeeper that walks each branch
    filling in reference parameters, floor placement, s-positions and
    dependent parameters, then places girders and records fork connections.
- **`pals_tree.{h,cpp}`** — the tree helpers every pipeline stage uses:
  reading and writing scalars, provenance-tracked copies, and finding a
  document's lists, a lattice's branches and a branch's line.
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
the same five trees `parse_and_expand_PALS` does without going through the disk:

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

### Measuring test coverage

With clang's source-based coverage, from the repository root (on macOS the
Apple clang in `/usr/bin` works; prefix the LLVM tools with `xcrun`):

```console
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate \
  -DCMAKE_SHARED_LINKER_FLAGS=-fprofile-instr-generate
cmake --build build-cov
LLVM_PROFILE_FILE=build-cov/cov-%p.profraw build-cov/tests/tests
llvm-profdata merge -o build-cov/cov.profdata build-cov/cov-*.profraw
llvm-cov report build-cov/tests/tests -object build-cov/libPALSParserCpp.dylib \
  -instr-profile=build-cov/cov.profdata src
```

`llvm-cov show` with the same arguments marks each line that never ran. The
library is `libPALSParserCpp.so` on Linux.

A Debug build also turns on rapidyaml's own checks, which a Release build
compiles out. They catch a node read as the wrong type -- `find_child` on a
list, say -- that a Release build would pass over silently, so run the tests
in a Debug build now and then.

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
