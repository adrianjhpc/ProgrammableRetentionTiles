# Hardware target plan

## AMD/Xilinx U280

Use the U280 as the first bandwidth-oriented target. Map retention classes to
independent HBM pseudo-channels so traffic and contention are observable. Keep
metadata in BRAM/URAM initially, then move bulk metadata to a reserved HBM region
behind a small on-chip cache.

Suggested progression:

1. synthesise the metadata controller by itself;
2. add an AXI4-Lite control interface and hardware trace generator;
3. attach three AXI masters to independent HBM channels;
4. add DMA-based migration between channels; and
5. expose counters over PCIe to a host application.

HBM is acting as a capacity and bandwidth substrate. Expiry and write-cost
differences are inserted by the controller; they are not properties of HBM.

## AMD/Xilinx U250

Use the U250 for portability and DDR-based experiments. Partition its external
memory into class regions and use arbitration to impose different per-class
latencies. It will not reproduce the U280's independent-bank bandwidth, but it
is useful for software, PCIe and controller validation.

## Intel Stratix 10

The exact device and board variant are required before binding external memory.
The metadata core itself is portable SystemVerilog. Use the Quartus synthesis
script with the precise device identifier, then replace AMD AXI infrastructure
with Avalon-MM adapters around the same command interface.

## Portable boundary

Keep these blocks vendor-neutral:

- retention metadata and expiry checks;
- placement and promotion policy;
- statistics counters;
- trace format; and
- bank command scheduling.

Limit vendor-specific code to memory controllers, PCIe, clock/reset and AXI or
Avalon adapters. This will make U280 versus Stratix 10 comparisons meaningful.

