# API Reference

The public interface is the C API declared in `PALSParserCpp.h`, together with
the expression evaluator in `pals_expression.h`. Everything below is generated
from the header doc comments by [Doxygen](https://www.doxygen.nl) and embedded
with [Breathe](https://breathe.readthedocs.io).

The C API is organized by task: parse and expand a lattice, then navigate,
inspect, edit, and serialize the resulting trees, with dedicated sections for
node correspondence, name matching, and parameter lookup.

:::{admonition} Ownership
:class: tip

Every handle, string, and list the API returns is owned by the caller. Trees
are freed with {cpp:func}`delete_tree`; the `char*` returned by the read and
emit functions with {cpp:func}`yaml_free_string`; and each aggregate result
with its matching `free_*` function. See
[Memory management](#memory-management).
:::

## Core types

The opaque handles and sentinel values shared across the API.

```{doxygentypedef} YAMLTreeHandle
:project: PALSParserCpp
```

```{doxygentypedef} YAMLNodeId
:project: PALSParserCpp
```

```{doxygendefine} YAML_NULL_ID
:project: PALSParserCpp
```

```{doxygendefine} YAML_END
:project: PALSParserCpp
```

## Parsing & expansion

Read PALS/YAML from disk or memory and expand a lattice into its five
representations (`original`, `combined`, `expanded`, `full_expanded`, `adjunct`).

```{doxygengroup} parse
:project: PALSParserCpp
:content-only:
:members:
```

## Node correspondence

Map a single logical node across the four trees produced by
{cpp:func}`parse_and_expand_PALS`.

```{doxygengroup} correspond
:project: PALSParserCpp
:content-only:
:members:
```

## Name matching

Resolve a PALS name-matching string to the nodes it selects.

```{doxygengroup} namematch
:project: PALSParserCpp
:content-only:
:members:
```

## Parameter values

Look up the stored value of a single element parameter, constant, or variable.

```{doxygengroup} param
:project: PALSParserCpp
:content-only:
:members:
```

## Navigation & inspection

Walk a tree, read node keys and values, and test node types.

```{doxygengroup} nav
:project: PALSParserCpp
:content-only:
:members:
```

## Building & editing

Create trees and add, set, copy, or remove nodes.

```{doxygengroup} edit
:project: PALSParserCpp
:content-only:
:members:
```

## Serialization

Emit a node or whole tree back to YAML text or a file.

```{doxygengroup} emit
:project: PALSParserCpp
:content-only:
:members:
```

## Memory management

Release the handles and strings the API returns. (The subsystem-specific
`free_*` functions live with their data above:
{cpp:func}`free_lattice_problems`, {cpp:func}`free_correspondence_map`, and
{cpp:func}`free_name_matches`.)

```{doxygengroup} lifecycle
:project: PALSParserCpp
:content-only:
:members:
```

## Expression evaluation

The recursive-descent evaluator behind the PALS expression grammar. The C entry
point {cpp:func}`evaluate_pals_expression` (declared in `PALSParserCpp.h`)
evaluates a standalone string; the `pals::` interface in `pals_expression.h` is
the C++ evaluator it and lattice expansion are built on.

```{doxygengroup} expr
:project: PALSParserCpp
:content-only:
:members:
```
