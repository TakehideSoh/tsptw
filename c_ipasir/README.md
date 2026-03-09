# SAT-based CEGAR Hamiltonian Cycle Solver (C / IPASIR)

A self-contained C solver for the Hamiltonian Cycle Problem using
**SAT-based CEGAR** (Counter-Example Guided Abstraction Refinement).
It links against [CaDiCaL](https://github.com/arminbiere/cadical) via
the standard [IPASIR](https://github.com/biotool-paper/ipasir) interface.

## Quick Start

### Prerequisites

- C compiler (GCC or Clang)
- C++ compiler (for building CaDiCaL)
- `make`, `bash`

macOS (Xcode Command Line Tools) and Linux are both supported.

### Build

```bash
tar xzf c_ipasir.tar.gz   # if starting from the tarball
./c_ipasir/build.sh
```

This will:
1. Build CaDiCaL from source (if needed)
2. Compile the solver binary → `c_ipasir/solver`

### Run

```bash
./c_ipasir/solver graph.col    # DIMACS .col format
./c_ipasir/solver graph.gb     # Stanford GraphBase format
```

Output ends with one of:
- `s SATISFIABLE` — Hamiltonian cycle found (tour is printed)
- `s UNSATISFIABLE` — no Hamiltonian cycle exists

## Input Formats

| Format | Extension | Description |
|--------|-----------|-------------|
| DIMACS | `.col`    | `p edge N M` header, `e u v` edges (1-indexed) |
| GraphBase | `.gb` | Stanford GraphBase text format |

## Algorithm

The solver uses a fixed configuration tuned for performance:

| Parameter | Value | Meaning |
|-----------|-------|---------|
| Encoding (`-e`) | 1 | Sinz at-most-one + at-least-one |
| Blocking (`-b`) | 3 | Separate in/out cut clauses |
| Two-opt (`-t`) | 3 | Merge sub-cycles via 2-opt |
| Loop prohibition (`-l`) | 1 | 2-cycle prohibition |

Each CEGAR iteration:
1. **Solve** the current SAT formula
2. **Extract** sub-cycles from the assignment
3. **Merge** sub-cycles via 2-opt where possible
4. **Block** remaining sub-cycles by adding cut clauses
5. Repeat until a single Hamiltonian cycle is found or UNSAT is proven

## File Structure

```
c_ipasir/
├── main.c                  # solver implementation
├── build.sh                # build script (Linux & macOS)
├── README.md               # this file
├── solver                  # built executable (generated)
└── third_party/
    └── cadical/            # CaDiCaL source (auto-downloaded if missing)
```

## Troubleshooting

**`error: built libcadical.a does not export ipasir_init`**
— A stale or cross-compiled `libcadical.a` exists. Delete it and rebuild:

```bash
rm -f c_ipasir/third_party/cadical/build/libcadical.a
./c_ipasir/build.sh
```

**Binary not executable on macOS**
— Pre-built binaries are platform-specific. Build from source with the
command above; Xcode Command Line Tools provide everything needed.
