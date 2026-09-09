slang-blocks
============

Extracts a per-level **block-diagram model** from an elaborated SystemVerilog
design and writes it as JSON. It draws nothing: rendering is a separate step,
done by `util/slang_hier_to_dot.py` in the `ibex` repo, which turns the same
JSON into Graphviz (SVG/PNG/PDF) or an editable draw.io file.

    slang-blocks --level xilinx_aida --level isolde_cluster --level isolde_tile \
                 -o model.json \
                 --top xilinx_aida --ignore-unknown-modules \
                 -f xilinx_aida_all_deps.f

    python3 util/slang_hier_to_dot.py --from-model model.json \
                 --outdir doc/diagrams --format svg,drawio

It accepts the standard slang driver options, so it takes the same command
files the synthesis flow already produces.

Why it is a separate tool
-------------------------

slang is **not forked**. This links the installed slang as a CMake package
(`find_package(slang)` / `slang::slang`), the way slang's own
`examples/integration` does, so there is nothing to rebase when slang moves.

The reason to have it at all is that the flow already pins one slang
(`SLANG_VERSION := release/11.0` in `Makefile.eda`, and a version check in the
ibex repo's `isolde/mk/slang-build.mk`). Doing the extraction with pyslang
means a second slang to keep in lockstep, plus a pip dependency in CI. This
tool is built from the same tree as the `slang` binary next to it.

Where the line is drawn
-----------------------

The model carries **facts only** — what the compiler knows:

- child module instances, with their resolved parameters;
- interface instances, and interface connections labelled with their modport;
- the level's own pins;
- every net endpoint, with its direction;
- modules with no definition (vendor IP, technology primitives), as black
  boxes;
- the assigns and procedural blocks written at the level, as one `glue` node;
- replication: an instance array or a `for` generate is reported once with a
  `count`, and connections are unioned over the replicas.

It contains **no drawing decisions**. Hiding clocks and resets, collapsing a
wide net into a bus node, how many signal names fit on an edge, colours,
box sizes — all of that lives in the renderer. That split is deliberate: the
presentation rules change often, the extraction does not.

Two limitations worth knowing, both inherited from the language rather than
from this tool:

- A module slang has no definition for is not bound at all, so its connection
  expressions are invalid. Names are recovered from the instantiation
  *syntax* instead, and only **named** port connections are recovered.
  Direction is left to the renderer to infer from the rest of the net.
- Interface ports resolve to the interface instance in the *parent* scope, so
  a level's own interface pins are mapped back by `(resolved path, modport)`.
  Without the modport in that key, several pins of one interface array
  collapse onto one.

Filtering: making it an abstract schematic
------------------------------------------

`--filter <file>` selects what the diagram shows. Three operations, applied in
file order:

    drop   <what> <regex>                 remove matching blocks / nets
    elide  <what> <regex>                 remove the block, but reconnect what
                                          ran through it
    group  <what> <regex> as "<name>"     collapse matches into one block

`<what>` is `inst` (instance name), `type` (module name), `net` (net name) or
`kind` (node kind: module, iface, blackbox, glue, port_in, ...).

**Elide is the operation that matters.** Dropping a block in the middle
silently disconnects its neighbours -- drop the data router and the core stops
connecting to memory at all, which is a lie. Eliding it removes the box but
emits an edge between what drove it and what it drove, labelled with the block
that was removed, so the path is still visible and still true. Those edges are
carried separately in the model (`edges`, with `via` and `bidir`) and the
renderer draws them dashed.

Interface connections elide correctly too: the modport direction, not the
order of the connection, decides which side is upstream. An address shim
between two interfaces disappears and leaves one bidirectional dashed link.

Grouping is what makes the result *abstract* rather than merely smaller: five
`i_mux_dm_sb_*` instances become one "DM slave-bus muxes x5" box, with every
connection rewritten onto it and duplicates merged.

See `examples/isolde_cluster.filter`. On `isolde_cluster` that file takes the
diagram from 23 instances to 13, with the three address shims replaced by
dashed "via i_dmem_shim" links between the muxed interface and the memory
port.

Schema
------

`slang-blocks-model/2` (v1 is still accepted by the renderer; v2 adds net
`width` and the `edges` list that elision produces). The Python renderer both
produces and consumes it, so it doubles as the reference implementation:

    # golden file from the reference implementation
    python3 util/slang_hier_to_dot.py --level ... --emit-model golden.json -- <slang args>

    # this tool, same design
    slang-blocks --level ... -o model.json <slang args>

Rendering both and comparing the output is the regression test.

Building
--------

From the repo root, with slang already installed by `Makefile.eda`:

    make -f Makefile.eda slang          # once
    make -f Makefile.eda slang-blocks

Or by hand:

    cmake -S . -B build -DCMAKE_PREFIX_PATH=<repo>/eda/slang
    cmake --build build -j
    cmake --install build

Requires a C++20 compiler and slang 11.x.
