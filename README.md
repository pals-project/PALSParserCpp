# PALSParserCpp

PALSParserCpp is a parser library for [PALS](https://github.com/pals-project/pals)
accelerator lattice files. It reads lattices into memory with
[rapidyaml](https://github.com/biojppm/rapidyaml), and lets a program navigate,
modify, print and write them, and find elements and parameters by PALS name
matching.

Its main job is lattice expansion as the PALS standard defines it: `include` and
`load`, inheritance, `set`s, expressions and controllers, branch expansion, and
bookkeeping that computes every dependent parameter (reference energy, floor
placement, s-positions, ...). `parse_and_expand_PALS` expands the lattice named
in the call or, if none is named, the one PALS chooses: the lattice in the last
`use` statement, or the last lattice defined if there is none. It returns five
trees describing the result, along with a list of the problems found.

The library has a C API, so other languages can call it. See
[Language interfaces](#language-interfaces).

## Building

You need CMake 3.18 or later and a C++17 compiler. CMake downloads the other
dependencies itself (rapidyaml, PCRE2, Catch2 and
[AtomicAndPhysicalConstantsCLib](https://github.com/pals-project/AtomicAndPhysicalConstantsCLib)).
From the repository root:

```console
cmake -S . -B build
cmake --build build
```

This builds the shared library `libPALSParserCpp` (`.dylib` on macOS, `.so` on
Linux) along with the tests and examples. To run the tests:

```console
ctest --test-dir build --output-on-failure
```

### Packaging and installed dependencies

CMake 3.18 or later and a C++17 compiler are required. The defaults build tests
and examples and fetch the dependencies. Packagers can instead use:

```console
cmake -S . -B build-package -DPALS_USE_SYSTEM_DEPS=ON -DBUILD_TESTING=OFF -DPALS_BUILD_EXAMPLES=OFF -DCMAKE_PREFIX_PATH=/path/to/dependencies -DCMAKE_INSTALL_PREFIX=/path/to/prefix
cmake --build build-package
cmake --install build-package --component PALSParserCpp
```

`PALS_USE_SYSTEM_DEPS=ON` disables dependency downloads. It requires rapidyaml's
`ryml` CMake package, PCRE2 (8-bit), and APC. When `BUILD_TESTING=ON`, it also
requires Catch2 3's CMake package. `BUILD_TESTING` and `PALS_BUILD_EXAMPLES`
independently control tests and examples; both default to `ON`.

Use `CMAKE_PREFIX_PATH` to locate installed dependencies, or set
`PALS_PCRE2_INCLUDE_DIR`, `PALS_PCRE2_LIBRARY`, `PALS_APC_INCLUDE_DIR`, and
`PALS_APC_LIBRARY` explicitly. `PALS_APC_LIBRARY` may select a static archive
built with position-independent code to bundle APC. For static PCRE2, set
`PALS_PCRE2_STATIC=ON` and point `PALS_PCRE2_LIBRARY` at the static library;
this supplies the `PCRE2_STATIC` definition required on Windows. The option
defaults to `OFF` for shared PCRE2.

The `PALSParserCpp` install component installs only the library, its public C
header, and an upstream license file if present. It excludes dependencies,
tests and examples. Shared dependencies must be deployed separately. On macOS
and Linux, installed libraries search their own directory for dependencies
(`@loader_path` and `$ORIGIN`, respectively). Dependencies in other directories
require an appropriate runtime search path supplied by the application or
packaging tool. On Windows, place dependency DLLs on the DLL search path.

## Examples

Both examples read their lattices from `lattice_files/` through a relative path,
so run them from `build/`.

### Reading and writing a lattice

`examples/example_rw.cpp` shows how to use the library to read a lattice file,
make basic changes (adding and removing elements, renaming, ...), print it to
the console, and write it back to a file. It reads `ex.pals.yaml` and writes
`expand.pals.yaml`. To run it:

```console
./example_rw
```

### Expanding a lattice

`examples/print_lattices.cpp` expands a lattice and prints the five trees. Its
first argument is the name of a file in `lattice_files/`. The optional
`-lat <name>` chooses the lattice to expand; without it, PALS chooses as
described above. The trees are:

- `original`: the file as written, along with every file it reaches by
  `include` or `load`.
- `combined`: the file with each included file substituted in and each loaded
  file merged in.
- `expanded`: the lattice after expansion, holding only the parameters the
  author wrote. Its values are the final ones, so a parameter in both this tree
  and `full_expanded` has the same value in both.
- `full_expanded`: the same lattice with every dependent parameter computed:
  reference parameters, floor placement, s-positions, and the derived members of
  each parameter family.
- `adjunct`: the rest of the document, such as element and beamline
  definitions, `use` statements, and any lattice that was not expanded.

To run it:

```console
./print_lattices ex.pals.yaml -lat lat2
```

## Language interfaces

These packages wrap the PALSParserCpp library for other languages. Each needs a
PALSParserCpp checkout built beside its own; see its installation guide.

- [PALSParserJ.jl](https://github.com/pals-project/PALSParserJ.jl) (Julia). It
  also translates PALS lattices to [Bmad](https://github.com/bmad-sim/bmad-ecosystem),
  [SciBmad](https://github.com/bmad-sim/SciBmad.jl) and
  [MAD-X](https://mad.web.cern.ch/mad/).
- [PALSParserPy](https://github.com/pals-project/PALSParserPy) (Python). It also
  translates PALS lattices to Bmad, MAD-X and
  [Xsuite](https://github.com/xsuite/xsuite).

## Documentation

The full documentation — the user guide, the five-tree model, the expression
evaluation rules, and the generated C API reference — is at
<https://pals-project.github.io/PALSParserCpp/>.

Notes for working on the library itself (source layout, memory model, running
the tests, measuring test coverage, building the docs) are on the
[Working on PALSParserCpp](https://pals-project.github.io/PALSParserCpp/develop.html)
page.
