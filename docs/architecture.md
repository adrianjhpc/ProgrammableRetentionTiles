# Architecture boundary

## Purpose

The prototype separates the system question from the device question.

The system question is whether data-lifetime information can reduce write cost
without creating excessive refresh, migration, metadata or reliability costs.
The device question is what latency, energy and error distributions a particular
low-retention MTJ array can actually deliver. The FPGA answers the first question
using parameterised models; it cannot answer the second.

## Logical line state

The first controller associates this state with every 64-byte logical line:

| Field | Purpose |
|---|---|
| valid | Distinguishes allocated data from reusable capacity |
| class | Selects the retention and access-cost model |
| last-write epoch | Computes age without decrementing every line each cycle |
| dirty | Needed when a backing tier is introduced |
| ECC status | Added when stochastic bit failures are modelled in RTL |

The checked-in RTL implements valid, class and last-write epoch. Dirty tracking,
ECC and physical-location tables belong in the next checkpoint.

## Command semantics

- **Write:** places a line in a requested class and resets its age.
- **Read:** succeeds only if the line remains inside its retention guarantee.
- **Refresh:** reads and rewrites the same physical class, resetting age.
- **Migrate:** reads one class and writes another, then releases the old slot.
- **Invalidate:** releases the line without preserving its contents.

Natural physical decay is never used as deallocation. The controller invalidates
dead data before its guarantee ends; otherwise it treats decay as a data error.

The U280 vertical slice implements the data movement half of `Migrate` as a
512-bit HLS copy between three HBM ports. Policy and expiry decisions remain in
the host runtime at this checkpoint. Moving those decisions beside the memory
is the next hardware boundary.

## Scaling metadata

The first RTL uses arrays with an explicit reset for clarity. That is appropriate
for a small proof but may prevent block-RAM inference at large sizes. The next
implementation should use one of:

1. a generation-tag scheme that avoids clearing the timestamp RAM;
2. a small resettable valid bitmap plus non-reset BRAM metadata; or
3. metadata stored in a reserved HBM/DDR region with a cache on the FPGA.

An exact per-line deadline queue is unnecessary. Bucket lines by expiry epoch in
a timer wheel and inspect only the bucket approaching its deadline.

## First experimental questions

1. What fraction of allocations can use the shortest class safely?
2. What is the refresh and migration traffic for imperfect lifetime predictions?
3. How much metadata cache bandwidth is required?
4. Does promotion happen early enough to avoid tail failures?
5. At what device write-energy ratio does the policy break even?
