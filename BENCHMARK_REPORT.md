# Exercise 3: Ring All-Reduce over RDMA

## Final Report

## 1. Project goal

The goal of this exercise was to build a single-threaded collective communication
library with the Verbs API and compare two transfer protocols:

- **Eager:** send data immediately into a receive buffer.
- **Rendezvous:** write data directly into registered remote memory.

The processes form a logical ring. The implementation supports 2 and 4 ranks
and provides Reduce Scatter, All Gather, and All Reduce.

## 2. What we built

Each rank owns two reliable-connected queue pairs:

- one QP sends to the next rank;
- one QP receives from the previous rank.

TCP is used only during setup and shutdown. It exchanges the QP number, PSN,
LID/GID, registered-memory address, rkey, rank, and group size. After this
exchange, each QP moves through `INIT -> RTR -> RTS`, and collective data moves
over RDMA.

The registered region has three logical parts:

```text
[ work vector | Reduce Scatter staging slots | eager receive buffer ]
```

The code is split by responsibility:

| Module | Responsibility |
| --- | --- |
| `ring_allreduce.c` | Public API and protocol selection |
| `pg_verbs.c` | RDMA resources, QPs, posting, and completion polling |
| `pg_bootstrap.c` | TCP metadata exchange and barriers |
| `pg_topology.c` | Ring neighbors |
| `pg_reduction.c` | Chunk geometry and CPU reduction |
| `pg_collective.c` | Reduce Scatter, All Gather, and pipelining |
| `test.c` | Correctness suite and benchmark driver |

## 3. Ring algorithm

An input vector is divided into one chunk per rank. When the count is uneven,
the first chunks receive one extra element.

### Reduce Scatter

Each rank begins with the complete local input. During `P - 1` rounds, every
rank sends one chunk clockwise and receives one from its previous neighbor.
The received data is reduced into the local copy. At the end, every rank owns
one fully reduced chunk.

### All Gather

During another `P - 1` rounds, those reduced chunks continue around the ring.
No further reduction is needed. At the end, every rank has the full reduced
vector.

In short:

```text
Reduce Scatter + All Gather = All Reduce
```

## 4. Transfer protocols

### Eager

The eager path uses `IBV_WR_SEND_WITH_IMM`. Chunks are split into 4 KiB
segments and received into a fixed registered buffer. The CPU then reduces or
copies each received segment.

This path is simple and is intended for small messages. Its cost grows quickly
for large vectors because it needs many small sends and receive-side copies.

### Rendezvous

The rendezvous path uses `IBV_WR_RDMA_WRITE_WITH_IMM` and 128 KiB segments.
During Reduce Scatter, each round writes into its own staging slot. During All
Gather, data is written directly to its final offset in the receiver's work
vector, avoiding an extra receive-side copy.

A posted receive is still required. RDMA Write itself does not notify the
remote CPU, but Write With Immediate consumes a receive work request and
produces a receive completion containing the immediate value.

### Automatic selection

Auto mode selects the protocol from the largest chunk size:

```text
largest chunk <= 16 KiB  -> eager
largest chunk >  16 KiB  -> rendezvous
```

Because every rank computes this from the same count and group size, all ranks
select the same protocol without another handshake.

### Pipelining

Rendezvous chunks are divided into 128 KiB segments. The pipelined path allows
several operations to be outstanding, bounded by the QP depth. A received
segment can be reduced and forwarded while later segments are still moving.

The `-nopipe` option keeps a step-synchronous rendezvous baseline for comparison.
Per-step staging slots prevent a later ring step from overwriting data that an
earlier step still needs.

## 5. Correctness validation

The final implementation was tested on the course Mellanox machines with both
2 and 4 ranks. The validation covered:

- `PG_INT32` and `PG_DOUBLE`;
- `PG_SUM` and `PG_PROD`;
- zero and one element;
- counts around `P - 1`, `P`, and `P + 1`;
- uneven chunks;
- eager and rendezvous segment boundaries;
- vectors up to 4 MiB;
- in-place All Reduce;
- standalone Reduce Scatter and All Gather;
- 50 back-to-back collectives;
- eager, rendezvous, auto, pipelined, and `-nopipe` modes;
- 2-rank and 4-rank ring connectivity.

The local phase tests and all distributed correctness scenarios completed
successfully before collecting the benchmark results.

## 6. Benchmark method

The benchmark used `PG_DOUBLE` with `PG_SUM`. Vector sizes are powers of two
from 8 bytes through 4 MiB.

For each size:

1. deterministic input was prepared before timing;
2. warm-up collectives were run;
3. only repeated calls to `pg_all_reduce()` were timed;
4. time was measured with `clock_gettime(CLOCK_MONOTONIC)`;
5. rank 0 printed average latency in microseconds.

Connection setup, memory registration, allocation, and bootstrap were outside
the timed section.

The 2-rank runs used the driver's adaptive defaults:

| Vector size | Measured iterations |
| --- | ---: |
| Up to 4 KiB | 2000 |
| Above 4 KiB through 256 KiB | 500 |
| Above 256 KiB | 100 |

The 4-rank runs used 10 measured iterations and 10 warm-ups per size. This was
necessary because forced eager mode became extremely slow with four ranks.
The smaller sample makes the 4-rank measurements noisier, so isolated spikes
must not be treated as stable performance claims.

## 7. Results

All values below are average latency in microseconds. The complete measurements
are preserved in the eight `results-*.tsv` files.

### 2 ranks

| Vector size | Eager | Rendezvous pipeline | Rendezvous no pipeline | Auto |
| ---: | ---: | ---: | ---: | ---: |
| 8 B | 85.145 | 94.457 | 99.514 | 102.427 |
| 4 KiB | 110.765 | 119.981 | 65.508 | 94.713 |
| 32 KiB | 592.356 | 28.679 | 35.458 | 410.359 |
| 64 KiB | 995.219 | 72.742 | 70.407 | 44.028 |
| 256 KiB | 5708.841 | 337.203 | 167.126 | 167.625 |
| 1 MiB | 25778.196 | 1360.294 | 1057.538 | 1181.901 |
| 4 MiB | 79620.084 | 6894.067 | 4391.254 | 6195.669 |

At 64 KiB, 1 MiB, and 4 MiB, pipelined rendezvous was approximately 13.7x,
19.0x, and 11.5x faster than forced eager respectively.

For two ranks, the no-pipeline path was faster than the pipeline at several
large sizes. At 4 MiB it measured 4.391 ms compared with 6.894 ms. With only
one neighbor step in each collective phase, there is less opportunity to hide
work, while pipeline bookkeeping still has a cost.

### 4 ranks

| Vector size | Eager | Rendezvous pipeline | Rendezvous no pipeline | Auto |
| ---: | ---: | ---: | ---: | ---: |
| 8 B | 32718.761 | 13208.369 | 32713.620 | 34807.128 |
| 4 KiB | 32296.655 | 13962.540 | 31248.594 | 30086.641 |
| 32 KiB | 65425.902 | 9695.779 | 33264.258 | 63326.354 |
| 64 KiB | 124256.598 | 76.230 | 20640.253 | 122258.853 |
| 256 KiB | 513470.161 | 223.635 | 16042.462 | 226.157 |
| 1 MiB | 2198999.742 | 956.332 | 1065.194 | 951.232 |
| 4 MiB | 8131506.861 | 19039.902 | 171682.253 | 19391.011 |

Forced eager scaled poorly in this run. At 1 MiB it measured about 2.199
seconds, compared with 0.956 ms for pipelined rendezvous. At 4 MiB the measured
latencies were 8.132 seconds and 19.040 ms respectively.

The 4-rank pipeline was 10.2% faster than `-nopipe` at 1 MiB and 88.9% faster
at 4 MiB. However, some other sizes show abrupt reversals. The 2 MiB pipelined
measurement, for example, was slower than `-nopipe`. With only 10 measured
iterations, these isolated values are best treated as noise or system
interference rather than a general algorithmic result.

## 8. What the results mean

### Eager is suitable only for small chunks

The eager implementation uses 4 KiB messages. A large vector therefore creates
many SEND operations and receive-side copies. This explains the sharp increase
in forced-eager latency, especially with four ranks and more ring rounds.

### Rendezvous is the practical large-message path

Rendezvous uses larger segments and RDMA writes. It avoids the eager bounce-copy
cost during All Gather and dramatically improves the measured large-message
latency in both rank configurations.

### Auto mode switches where expected

The threshold is applied to the largest chunk, not the whole vector. For double
data in these power-of-two tests:

- with 2 ranks, 32 KiB total gives a 16 KiB chunk and remains eager; 64 KiB is
  the first tested vector that uses rendezvous;
- with 4 ranks, 64 KiB total gives a 16 KiB chunk and remains eager; 128 KiB is
  the first tested vector that uses rendezvous.

The large latency drop in each auto dataset appears at those boundaries. This
confirms that all ranks made the same deterministic protocol choice.

### Pipelining depends on available overlap

Pipelining is not free. With two ranks, each phase has only one ring step, and
the measured no-pipeline path was often better. With four ranks there are three
steps per phase, so overlap has more opportunity to help; the 1 MiB and 4 MiB
results show that benefit. More repetitions would be required for a stronger
statement at every size.

## 9. Limitations

- The 4-rank measurements use only 10 timed iterations and contain large
  fluctuations.
- Results were collected on shared course machines, so scheduling and other
  users may affect latency.
- The benchmark reports only rank-0 elapsed time and does not include variance
  or confidence intervals.
- The 2-rank and 4-rank runs used different iteration counts. Comparisons
  between protocols within one rank configuration are more reliable than exact
  comparisons between the two configurations.
- The chosen 16 KiB threshold follows the design baseline; this benchmark was
  not intended as a full threshold-tuning experiment.

These limitations do not affect correctness. They only limit how strongly we
interpret individual performance points.

## 10. Interview summary

A short explanation of the complete design is:

1. TCP creates the ring and exchanges RDMA addressing information.
2. Two RC QPs connect every rank to its previous and next neighbors.
3. Reduce Scatter reduces one rotating chunk per round.
4. All Gather rotates the final chunks until every rank has the complete result.
5. Eager uses SEND_WITH_IMM and a receive buffer for small chunks.
6. Rendezvous uses WRITE_WITH_IMM, staging slots, and direct final placement for
   large chunks.
7. Immediate data identifies the collective, round, and segment, while a posted
   receive provides the remote completion event.
8. Pipelining overlaps segment movement with CPU reduction, bounded by QP depth.
9. Auto mode makes the same protocol decision independently on every rank from
   the largest chunk size.

The main practical result is simple: eager provides the small-message baseline,
while rendezvous is essential for large vectors. Pipelining becomes most useful
when the ring has enough steps and data to overlap.

## 11. Raw result files

- `results-2-eager.tsv`
- `results-2-rdvz.tsv`
- `results-2-rdvz-nopipe.tsv`
- `results-2-auto.tsv`
- `results-4-eager.tsv`
- `results-4-rdvz.tsv`
- `results-4-rdvz-nopipe.tsv`
- `results-4-auto.tsv`
