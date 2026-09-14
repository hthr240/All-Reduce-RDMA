# Phase 10: Final Architecture

## Goal

Phase 10 simplified the completed project without changing its public API or
collective behavior. The full-mark reference remains the algorithmic baseline,
but this project keeps a small modular layout instead of one large source file.

## Final structure

The library has four production implementation modules:

| Module | Responsibility |
| --- | --- |
| `ring_allreduce.c` | Public API, process-group lifecycle, protocol selection, standalone CLI, and logging |
| `pg_bootstrap.c` | Host parsing, ring topology, metadata serialization, TCP bootstrap, and barriers |
| `pg_verbs.c` | Device, PD, MR, CQ, QP lifecycle, work requests, completions, and token test |
| `pg_collective.c` | Datatypes, chunk geometry, reduction kernels, and the unified collective progress engine |

Supporting files are intentionally separate:

- `pg.h` is the stable public API.
- `pg_internal.h` contains private state, constants, and internal declarations.
- `test.c` is the course-facing correctness and benchmark driver.
- `tests/` contains focused local tests that link the production modules normally.
- `BENCHMARK_REPORT.md` and `results-*.tsv` contain the final evidence.

## Mental model

```text
application / test.c
        |
        v
ring_allreduce.c       public API and lifecycle
        |
        +----> pg_bootstrap.c    build and synchronize the ring
        +----> pg_verbs.c        perform low-level RDMA operations
        +----> pg_collective.c   run Reduce Scatter and All Gather
```

`pg_internal.h` is shared only inside the implementation and tests. Normal
callers include only `pg.h` and use the opaque `void *` process-group handle.

## Main simplifications

1. Process-group setup now has one path. The standalone executable and course
   driver both build the same encoded group specification and call
   `connect_process_group()` once.
2. Topology is owned by bootstrap, rather than a separate one-function module.
3. Eager and rendezvous use one schedule-driven collective engine. The engine
   covers both Reduce Scatter and All Gather with global ring steps.
4. Reduction and chunk geometry live beside the collective algorithm that uses
   them.
5. CLI and logging no longer require separate implementation modules.
6. Tests link the four modules normally instead of including a production `.c`
   file.
7. Obsolete aliases for the directional QPs and CQs were removed.

## Reference alignment

The refactor preserves the behavior that matters for the exercise:

- one-based `-myindex` input converted once to a zero-based rank;
- a logical ring tested with 2 and 4 ranks;
- two directional RC QPs and split send/receive completion queues;
- QP transitions through `INIT -> RTR -> RTS`;
- fixed-width TCP metadata containing QPN, PSN, LID/GID, address, and rkey;
- eager `SEND_WITH_IMM` with a 4 KiB registered receive buffer;
- rendezvous `RDMA_WRITE_WITH_IMM` with 128 KiB segments;
- one staging slot per Reduce Scatter step;
- direct final placement during rendezvous All Gather;
- optional rendezvous pipelining and a `-nopipe` baseline;
- deterministic auto selection from the largest chunk size;
- immediate tags containing collective sequence, step, and segment;
- `PG_INT32`, `PG_DOUBLE`, `PG_SUM`, and `PG_PROD`;
- uneven, empty, in-place, repeated, and maximum-size collectives;
- rank-0-only TSV benchmark output.

Differences from the reference are deliberate implementation choices, not
missing features. The binary metadata format, split CQs, 4 KiB eager segments,
and modular source layout remain because they are correct, tested, and easy to
explain.

## Validation

Focused phase tests and distributed NIC checks protect each changed boundary.
The final four-module validation is:

```sh
make clean
make
make check
./test -help
```

Then run the 2-rank and 4-rank suites in all modes:

```sh
./test ... -suite -mode eager
./test ... -suite -mode rdvz
./test ... -suite -mode rdvz -nopipe
./test ... -suite -mode auto
```

The final submission check also includes the token test, 50 repeated calls,
in-place and uneven counts, a 4 MiB rendezvous case, complete benchmark files,
and a clean rebuild from the submitted archive.
