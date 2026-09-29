# Shared-style experiment

The user's acceptance condition is measured S3 frame latency with Bouncing Balls
JSX, including worst frames and missed 16.67 ms budgets. The experimental engine
must also reduce total live storage, including style records and ownership data.
A smaller `sizeof(Node)` alone is insufficient. No package publication is planned.

## Proposed implementation scope

Keep links, layout memo, previous geometry, render state, tag IDs and text handles
as they are. Replace only the embedded computed-style record with a shared record
pointer in an experimental build. Keep the inline representation for a matched
control build; both builds compile the same app, engine code and instrumentation.
The A/B switch is a development tool, not an application opt-in requirement.

Native `Node.style` reads become a const `Node.style()` reference. Group repeated
reads in each operation so the compiler can reuse the loaded pointer. Explicit
`mutableStyle()` calls detach only when the record has another owner. Do not
return mutable references from read APIs. Intern equal stable computed styles at
style-recomputation boundaries, not on every frame or every field read. Initially
exclude records with separately owned rare-style handles from interning. Retain
correct rare-style cloning and destruction. Use semantic field equality before
engine integration; the contained prototype currently uses byte equality, which
can miss sharing due to padding but cannot merge different byte representations.

Migration scope is the native engine/elements call sites and their native tests
listed in `build/css-feature-audit/shared-style-scope.json`, plus target diagnostics
that inspect Node.style. No layout accessor migration is included. The earlier
63-file style-and-layout proposal remains withdrawn.

Const compilation must identify every write and reference escape. Then test
copy/move/reuse, CSS inheritance, inline overrides, class changes, rare-style
ownership, pixel/geometry equivalence, allocation counts, and original JSX
bouncing-ball behavior. Only a passing candidate is eligible for the device A/B.
Restore the qualified pedal image after device experiments.

## Completed contained prototype

`tests/ui_shared_style_prototype.cpp` leaves the framework unchanged. ASan/UBSan
passes copy-on-write isolation, moves, self-assignment, 128,000 dynamic writes and
complete record reclamation. With the pedal's 80-byte ComputedStyle on the host:

| Workload                              | Handle bytes | Record bytes | Inline style bytes |
| ------------------------------------- | -----------: | -----------: | -----------------: |
| 160 nodes, 10 equal style groups      |        1,280 |        1,040 |             12,800 |
| Same nodes, 64 independently modified |        1,280 |        7,696 |             12,800 |

These are requested payload bytes on a 64-bit host, excluding allocator overhead
and the common default record, **not measured S3 heap savings or frame timings**.
Whole-style sharing does not combine nodes with different positions. The final
engine census must include every supporting allocation and the default record.

## Device baseline setup

`scripts/build-ui-benchmark.mjs` builds the normal pedal firmware with a temporary
benchmark JSX entry and restores package.json in a finally block. OTA, partition
layout and native audio services remain available. The workload follows the
64-ball JSX example's seeded motion, position/color updates and half-second FPS
badge, using this pedal's Saira font and 600 x 450 canvas at CSS ratio 2. It targets
60 fps without vsync. Font and display differences from the original example are
intentional and identical for the planned A/B.

A target hook records scheduler work including display flush and completion
cadence, skips 300 warm-up frames and collects 3,600 frames. Histograms give 250 us
upper bounds for p99; maximum and over-budget counts are exact. A single summary
prints after collection. The initial build still has upstream perf logging on;
that must be disabled for the clean comparison. Scheduler completion is not a
photodiode measurement of panel scanout.

The first baseline showed about 13 fps, before any shared-style change. The
60-fps condition is therefore not established. Do not present this baseline or
the contained ownership test as evidence that shared styles sustain 60 fps.

## Approval-review status

Automatic review rejected applying the broad native migration on 2026-09-29,
citing the previous pointer-indirection restriction and unverified API/mutation
semantics. No migration was applied. The specific pending action is implementing
the style-only experiment above, including the native caller migration, then
testing it against the preserved inline baseline. The user's newer openness to
sharing is recorded; the review still requires explicit approval of this scope.
