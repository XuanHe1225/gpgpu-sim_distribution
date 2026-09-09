# Optional memory events for PNMServing

Set `PNMSERVING_MEMORY_EVENTS=/absolute/path/events.csv` when running Accel-Sim
to record memory observations. Leave it unset to disable recording. The sink
accepts a regular file or a FIFO; compression can run in a separate process.

The observer records warp binding, instruction issue and completion callbacks,
active global lane addresses, request creation/status/release, MSHR merges, L2
access status, FR-FCFS selection, and issued ACT/PRE/RD/WR commands. It observes
the existing simulator decisions without changing queues, addresses or timing.

The CSV header defines the fields. `cycle` is the cumulative shader cycle;
`dram_cycle` uses the controller's command counter for DRAM commands. `inst`
identifies a dynamic warp instruction. `request`, `parent` and `related`
connect request lifetimes, splits and MSHR merges. For a merge, `related` is
the existing MSHR leader. Initial requests from the same instruction share
`inst`; they are not necessarily parent/child requests. `channel`, `bank`,
`row`, and `col` are decoded simulator coordinates.

`warp_bind` uses `sm`, `related` (dynamic warp), `bytes` (warp slot), `address`
(CTA ID), and `aux` (kernel ID). Completion is a simulator callback and need
not correspond one-to-one with a SASS instruction. Lane logging covers the
scalar/vector global accesses in the SM80 experiment; it is not a general
description of TMA, texture or surface operands.

Validate logging by replaying identical traces with the sink off and on and
comparing cycles, instructions, cache and DRAM counters. The upstream
`dram_t::n_ref` field is uninitialized and never updated; preserve its raw
printed value but exclude `n_ref_event` from this comparison. Simulated
addresses and controller labels do not establish physical A100 HBM wiring.
