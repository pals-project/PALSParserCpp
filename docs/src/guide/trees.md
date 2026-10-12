# The five trees

`parse_and_expand_PALS` does not return *the* lattice. It returns five YAML
trees, each a complete, independently owned view of the same document at a
different stage of its derivation. Which one to read depends on the question
being asked: what the file says, what the author wrote, or what the lattice
actually is.

The PALS standard deliberately leaves this open — it says what lattice expansion
must produce, not how a parser must hold the result. The five trees are this
library's answer, and everything on this page is PALSParserCpp's own arrangement
rather than part of the standard.

| Tree | Holds |
| --- | --- |
| `original` | every file, verbatim, keyed by path |
| `combined` | those files spliced into one document |
| `full_expanded` | one lattice, expanded, with every computed value |
| `expanded` | the same lattice, back to the author's inputs |
| `adjunct` | everything the expanded trees do not carry |

All five are `YAMLTreeHandle`s, and each must be freed with `delete_tree`.
Freeing one leaves the other four intact.

## A worked example

```yaml
PALS:
  facility:
    - constants:
        k_q: 0.32
    - begin:
        kind: BeginningEle
        ReferenceP:
          species_ref: "electron"
          E_tot_ref: 1.0e9
    - d1: {kind: Drift, length: 2.0}
    - q1:
        kind: Quadrupole
        length: 0.5
        MagneticMultipoleP:
          Kn1: k_q
    - cell:
        kind: BeamLine
        line: [d1, q1]
    - main:
        kind: BeamLine
        line:
          - begin
          - cell: {repeat: 2}
    - lat1: {kind: Lattice, branches: [main]}
    - use: "lat1"
```

### `original` and `combined`

`original` maps each file the document is built from — the top-level file and
every file it reaches by `include` or `load`, at any depth — to its unparsed
contents, keyed by path. Nothing is resolved and nothing is evaluated: an
expression is still its text, and `Kn1` is still `k_q`. For a single-file
document it is that one file under its own name.

`combined` is the same content as one document: every `include` spliced inline
where the directive stood, and every `load`ed file merged in subnode by subnode
under the `PALS` root. It is still unevaluated — this is the document as though
it had been written as one file.

### `full_expanded`

The selected lattice, expanded, and nothing else:

```yaml
lat1:
  kind: Lattice
  branches:
    - main:
        line:
          - begin:
              kind: BeginningEle
              ReferenceP:
                species_ref: electron
                E_tot_ref: 1e+09
                pc_ref: 999999869.4400277
                time_ref: 0
              FloorP: {x: 0, y: 0, z: 0, theta: 0, phi: -0, psi: 0}
              s_position: 0
              element_index: 1
          - d1:
              kind: Drift
              length: 2
              # ... ReferenceP, FloorP ...
              s_position: 0
              element_index: 2
          - q1:
              kind: Quadrupole
              length: 0.5
              MagneticMultipoleP:
                Kn1: 0.32
                Kn1L: 0.16
                Bn1: -1.0674049652737057
                Bn1L: -0.5337024826368528
              # ... ReferenceP, FloorP ...
              s_position: 2
              element_index: 3
          # ... the second copy of the cell: d1, q1 ...
          - branch_end:
              kind: Placeholder
              # ... ReferenceP, FloorP ...
              s_position: 5
              element_index: 6
```

Note what the shape is, before the values. The tree is rooted at the lattice
entry itself, stripped of the `PALS`/`facility` scaffolding it was defined
under, so its root map holds the single `lat1:` entry. The `branches:` entries
are **branches, not the BeamLines they were built from**, and so carry no
`kind:` — `main` was a `kind: BeamLine` definition in the file and is a branch
here. A BeamLine referenced inside a `line:` is a sub-line whose contents are
spliced straight into the enclosing line, so no nested BeamLine survives either:
`cell` is gone and its two elements appear twice, flat, where the `repeat: 2`
put them.

### What the expansion adds

Everything below is written by PALSParserCpp into `full_expanded` and appears in no
other tree.

**Per element, from the element bookkeeper:**

- `element_index` — the element's position in the branch line that holds it,
  counting from one. Every entry of the line is counted, so this is the array
  index the element sits at. It restarts at one in each branch, and it is a
  position rather than an identity: the two `d1` copies above are indices 2 and
  4.
- `s_position` — the longitudinal position of the element's upstream end.
- `ReferenceP` — the reference species, energy, momentum and time, threaded
  along the branch from the beginning element (or from the `Fork` that created
  the branch) and propagated through each element in turn.
- `FloorP` — the element's nominal floor placement, accumulated along the
  branch. A `FloorShift` places its exit end, and so everything downstream of
  it, from its `CoordinateSetP` and `BodyShiftP`; a `Fiducial` fixes its own
  placement the same way and carries the stretch of the branch around it —
  from the branch start or the previous `FloorShift` to the next one — bodily
  to match.
- `ShiftedFloorP` — the placement with the element's `BodyShiftP` and the
  shifts of the `Girder`s supporting it applied. Equal to `FloorP` for an
  element nothing shifts.
- The **derived members of every parameter family** the element uses: an
  authored `Kn1` brings `Kn1L`, `Bn1` and `Bn1L` with it, a `gradient` brings a
  `voltage`, an authored pair of bend geometry parameters brings the rest.
- The **non-zero defaults** of every parameter group the element carries.
  Presence of the group is what triggers this: a group the element does not
  carry is not materialized.

**Per branch:**

- A `branch_end` element, a zero-length `Placeholder` appended after the last
  real element of every non-empty branch. It exists to hold the branch's final
  state — the reference and floor at the downstream end of the last element —
  which nothing else in the tree records. It is an element of the line like any
  other, so it is numbered with the rest (index 6 above).

**Where the lattice's own structure needs recording:**

- `multipass_index` on the elements of a `multipass` line, giving the pass
  number: how many times a particle will have travelled through that physical
  element by that point.
- `ForkP.forked_to` on each resolved `Fork`, naming the destination it actually
  reached as `{branch}>>{element}`, and a matching `ForkFromP` back-reference on
  that destination, one entry per incoming Fork.

Every expression in the tree has been evaluated to a number, every `set`
executed, and every ABSOLUTE controller applied to the parameters it drives.
See [Evaluating expressions](expressions.md) for when each of those happens.

### `expanded`

The same lattice with all of that removed:

```yaml
lat1:
  kind: Lattice
  branches:
    - main:
        line:
          - begin:
              kind: BeginningEle
              ReferenceP:
                species_ref: electron
                E_tot_ref: 1e+09
          - d1: {kind: Drift, length: 2}
          - q1:
              kind: Quadrupole
              length: 0.5
              MagneticMultipoleP: {Kn1: 0.32}
          - d1: {kind: Drift, length: 2}
          - q1:
              kind: Quadrupole
              length: 0.5
              MagneticMultipoleP: {Kn1: 0.32}
```

What stays is decided by **what the author wrote**, recorded as a snapshot of
the lattice's shape taken at the last moment it held the inputs and nothing
else: after expansion and expression evaluation, before the bookkeeper. A
parameter is kept when it was present in that snapshot or was written by a
`post_expansion` `set`; everything else the bookkeeper computed is cut,
along with any group left empty by the cutting and the `branch_end` elements.
`kind` is always kept — it says what an element is, not how it was set up.

Two consequences worth being clear about:

- The **values are the finished ones**. This is `full_expanded` with nodes
  removed, not a snapshot taken earlier, so a parameter present in both trees
  carries the same value in both — expressions evaluated, sets applied,
  ABSOLUTE controllers applied. `Kn1` reads `0.32` here, not `k_q`.
- The distinction is *authored versus computed*, not *input versus output*. A
  `multipass_index` survives into `expanded` because it is stamped during
  expansion, before the snapshot; an `element_index` does not, because the
  bookkeeper writes it after.

Use `expanded` to see what was asked for rather than what it implies, or to
write a lattice back out without the derived values.

### `adjunct`

Everything the expanded trees do not carry, keeping the `PALS`/`facility`
scaffolding it was written under: element and beamline definitions, `use`
statements, constants and variables, controllers, `set` commands, the
`post_expansion` and `adjustments` lists, and any `Lattice` that was not the one
expanded. The girders live here too, under `PALS: girders`, since they are
outside the lattice; each carries the `FloorP` and `ShiftedFloorP` the
expansion computed for it.

A definition substituted into the lattice is *copied* rather than moved, so it
appears in both places: `d1` above is in the expanded lattice twice and still
standing in `adjunct` once, as the definition those two copies were made from.

## Choosing a tree

- **What does this lattice actually do?** `full_expanded`. It is the only tree
  that carries a dependent parameter, and the only one where each copy of a
  repeated element is a separate element with its own position and reference.
- **What did the author write?** `expanded` for the lattice, `adjunct` for
  everything around it — the constants, the definitions, the controllers.
- **Where in the file did this come from?** `original` and `combined`, which
  alone keep the expression text and the file structure.
- **Writing a lattice back out?** `expanded` plus `adjunct`.

`get_lattice_parameter_value` takes the pair that between them hold every value
a lattice has: `full_expanded` for element parameters and `adjunct` for
constants, variables and unused definitions.

## Tracking a node across the trees

`build_correspondence_map` links a node to its counterparts in the other trees.
The link is recorded as each tree is derived from the one before it, not
reconstructed afterwards by matching paths or names, so it survives the
rearrangement that expansion does: one definition in `combined` maps to every
copy expansion made of it.

`original`, `combined`, `full_expanded` and `adjunct` take part. `expanded`
does not: it is a pruned copy of `full_expanded` rather than a derivation of its
own, so its nodes are found by path.

A node the expansion *created* — a `ReferenceP`, an `element_index`, a
`branch_end` — has no counterpart anywhere upstream and so appears in no link.
That is the correspondence map's answer to "where did this come from": nowhere,
it was computed.

## Problems

Expansion does not abort on a recoverable problem. Instead it leaves the
offending value as it found it and appends a message to the `problems` list the
`lattices` struct also carries, so the five trees come back expanded as far as
they could be. The list is empty when expansion was clean, and the caller owns
it — release it with `free_lattice_problems`.

The library never prints. Whether to report, save, or ignore the messages is the
caller's decision. What is reported is set out in
[Problems found during expansion](usage.md#problems-found-during-expansion).
