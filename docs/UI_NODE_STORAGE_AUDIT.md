# Whole-record UI storage audit

Audited on 2026-09-29. This batch keeps hot `Node`, `ComputedStyle`, and
`LayoutBox` fields directly addressable. There is no style/layout accessor
migration, packed-field decoding, or shared-style lookup. It is a checkpoint
toward the smaller-node goal, not a claim that the approximately 50-byte goal
has been reached or that every possible redesign has been exhausted.

## Latest rule-plan/reset experiment

The direct Node remains 144 B. The combined batch reduces temporary rule plans
from 1,012 to 228 B, cached plan entries from 612 to 598 B, and the S3
style-recomputation stack frame from 2,320 to 592 B. Resetting a style no longer
constructs a temporary Node. Automatic analysis omits generated pseudo-elements
only when unused; first-line and animation support remain independent.

Firmware is 3,776 B smaller than the qualified 148-byte image. Matched free PSRAM
increases 66,996 B, including a 65,536 B mapping-reservation page; the remaining
observed difference is 1,460 B. Host allocated bytes are unchanged. Task stack
reservations are unchanged, so stack-frame savings are not heap savings.

All native behavior checks and five device DSP/USB-error/watchdog checks pass.
However, the 90-second UI stress run pads/trims 241 / 320 audio frames, while the
fresh qualified control repeats 99 / 145. The batch is experimental and the
qualified image has been restored. First-paint medians improve in the three
short windows, but the longer control run paints faster than the candidate;
no repeatable startup speedup is established. Full measurements and artifact
hashes are in UI_MEMORY_OPTIMIZATION.md. No package was published or pushed.

## Supporting-record experiment

Optional data, CSS rules, keyframes and selector plans now have a tested compact
candidate (64 / 26 / 12 / 216 B). It saves 7,776 B of observed device PSRAM but
adds 1,440 B of firmware and records two startup audio deadline misses. Three
matched control repeats pass. It is not qualified; see UI_MEMORY_OPTIMIZATION.md.
Node and ordinary style fields remain directly stored at 148 / 80 B. The saved
class-capacity image below remains the accepted fallback.

## Current qualified development checkpoint

Node remains 148 B, computed style 80 B and TreeState 27,256 B. The combined
supporting-record/animation batch reduces optional data to 64 B, CSS rules to
24 B, keyframe rules to 12 B and selector plans to 216 B. Automatic analysis
eliminates unused CSS animation tracking and polling while preserving frame
callbacks. Normal style fields remain directly embedded with no added pointer.

Against the saved class-capacity control, firmware shrinks 44,816 B and measured
free PSRAM rises 13,248 B. Static PSRAM BSS is 1,616 B smaller; do not add that
saving to the heap delta. The matched populated host fixture separately saves
528 B. Three short starts, the long UI/audio soak and fixed audio window all
pass with zero misses/errors/watchdog events. Median first paint is 3.833312 s
versus 3.879942 s for fresh control repeats; steady audio time is unchanged.
See UI_MEMORY_OPTIMIZATION.md for evidence and qualification limits.

The record-only, original position-only and unpinned float-scan images remain
unqualified for their timing failures or variation. The combined build is a
15-fps development qualification, not 60-fps or hardware audio-null-test proof.
The approximately 50-byte goal and final registry-only build remain open.

## Unqualified position-storage follow-up

The linked S3 Node is 148 B, down from 156 B. ComputedStyle is 80 B rather than
88 B, and the 160-slot TreeState is 27,896 B rather than 29,176 B. Automatic
source analysis removes bottom pixels and top/right/bottom percentages for this
app. Native inputs, lists, unknown styles and logical insets retain support.
The four live position reads still compile to one `l16si`; 24 other field probes
retain their instruction sequences. There is no runtime edge mapping.

Cached style operations omit their unused transform payload, shrinking 48 to
24 B. Full-transform builds retain that payload and pass transform replay tests.
The inline cache saves 384 B of static PSRAM. Firmware shrinks 2,976 B; internal
sections and the flash mapping reservation are unchanged. The complete host
allocation fixture saves 4,096 B in every phase with matching pixel hashes.
Device timing and heap samples are recorded in UI_MEMORY_OPTIMIZATION.md.

All 130 position cases match the saved engine. Four partial-storage variants,
the pedal's 66 retained cases, 22 existing pixel frames and 26 repaint scenarios
pass. Nested layout still performs 4,544 calls across 84 cases. Five upstream
target diagnostic reads now use compile-time edge selection; older engines keep
the original arrays. That target compatibility change must accompany the later
registry release. No package was published for this checkpoint.

## Previous CSS pool follow-up

The Node stays 156 B with direct embedded fields. Associated S3 CSS variable
entries shrink 24 → 20 B, parsed-color cache entries 12 → 6 B, and compiled
CSS declarations 84 → 72 B. Packed-color narrowing preserves all target bits;
full-color targets retain 32-bit values. Compiled records reuse existing scalar
slots and their auxiliary count byte. There is no additional read indirection.

The fixed tree remains 29,176 B. Matched maintenance samples measure 12,488 B
more free PSRAM with unchanged internal SRAM, mapping reservation and static
BSS. Firmware grows 32 B. Host allocations save 256 B in the populated UI fixture
and 2,048 B in the independently modified variable-copy fixture. The proposed
shared-map ownership was removed because its latter case regressed by 512 B.
Neither custom allocator follow-up was applied. Both the 90-second UI/audio soak and 45-second fixed-load window have zero
DSP misses, USB errors and watchdog events; measured stage times are effectively
unchanged. See UI_MEMORY_OPTIMIZATION.md for checks, limitations and device timing; the approximately 50-byte goal is open.

## Consolidated changes

- Store listeners in one tagged collection with a live-type mask, replacing
  seven vector headers per optional record. Event aliases, order, mutation
  during dispatch, bubbling and global listener counts retain their semantics.
  The dispatcher reacquires storage after callbacks, which may remove a node
  or reallocate the collection. Tag filtering happens during event dispatch;
  it does not add work to computed-style field reads.
- Allocate each attribute name/value together at its actual bounded length.
  Growing one attribute preserves pointers to every other attribute. The
  existing 31-character name and 63-character value limits remain. Copies own
  independent buffers; overlapping input is handled before freeing storage.
- Empty authored-style override stores hold one pointer and allocate nothing.
  A populated store uses one contiguous block for its metadata and entries.
  This trades more small allocations on styled nodes for much less reserved
  empty storage. The allocator census counts those allocations and their
  rounding; it is not a sizeof-only comparison.
- Allocate virtual-list bookkeeping only on virtual-list nodes. The element
  backend changes alongside the engine. Scroll geometry stays 32-bit, including
  the 1,295,000-pixel regression case; ordinary nodes allocate no list state.
- Use the existing automatic renderer analysis to remove unreachable gradient
  and repeating-background payloads from RareStyle. Unknown/dynamic use retains
  support. Full-feature builds still exercise positive gradient cases.
- Keep both layout memo entries, but share their result dimensions. An old
  entry whose dimensions differ from the current result cannot hit; discard
  it rather than retaining another result pair. External flex resizing is
  still checked against the result before a hit. Pass tags wrap with explicit
  invalidation; alternating-cache and two full-wrap tests remain.
- Compile out transform-dirty storage when transforms are unreachable. That
  byte currently becomes alignment padding; no separate sizeof saving is
  attributed to it.

## First consolidated checkpoint (historical)

| Record                            | Previous reported checkpoint | This batch |
| --------------------------------- | ---------------------------: | ---------: |
| Node                              |                        220 B |      212 B |
| ComputedStyle                     |                        108 B |      108 B |
| LayoutBox                         |                         72 B |       64 B |
| RenderState                       |                         18 B |       18 B |
| TreeState, capacity 160           |                     39,472 B |   38,192 B |
| RareStyle                         |                        128 B |       52 B |
| Optional NodeRareData             |                        244 B |       72 B |
| One authored-style override store |                         36 B |        4 B |

The 220-byte checkpoint precedes the 16-bit memo tags and first-line pruning.
Immediately before the consolidated batch, Node was 216 B, LayoutBox 68 B,
TreeState 38,832 B, and NodeRareData 220 B. Those intermediate results are kept
in the raw artifacts, not presented as separate optimization/release steps.

These sizes come from the linked firmware DWARF and a separate probe using the
actual target compile flags. Width, background, text color, corner radius and
memo probes each compile to one aligned S3 field load.

## Direct 144-byte layout (experimental; timing tradeoff unresolved)

Automatic whole-source analysis now proves that this application's text and border
colors are always opaque, including its store getters and palette. The two alpha
members become constants. Unknown writes, receivers, object escapes, palette
mutation, native controls and older analysis retain storage. No application flags
or manual opt-in are involved.

| Occupied storage                          | Bytes | Layout                                              |
| ----------------------------------------- | ----: | --------------------------------------------------- |
| Computed-style data                       |    78 | Still begins at offset zero; nominal sizeof is 80   |
| Tag, five links, rare handle, text handle |    18 | Tag uses the two style tail-padding bytes           |
| Layout data                               |    37 | Nominal sizeof is 38                                |
| Node kind                                 |     1 | Uses the layout tail-padding byte                   |
| Render state                              |    10 | Directly embedded                                   |
| Total                                     |   144 | 160-slot TreeState is 26,616 B, previously 27,256 B |

The node kind is an unsigned byte; all declared kinds are 0–7. Its standalone
S3 read loses a sign-extension instruction. Existing field loads, the eight-field
sample and passing a style record to a helper retain their instruction sequences.
There is no style pointer, accessor dispatch or packed-field decoding.

Copy, move and reset tests protect the neighboring tag/kind fields from member
assignment. Full and pruned rendered frames match; text/border transparency is
also checked with each field independently retained. Host allocation totals do
not decrease in the matched fixture despite the smaller requested tree allocation;
allocator size classes matter. Device heap measurements are recorded separately.

Two preliminary variants place text before style. Both preserve pixels and audio
deadlines but regress median first paint by 1.89% and 4.90%; they are diagnostic
experiments, not successful speed optimizations. The style-first correction keeps
the style record at its original offset and retains the 144-byte total.
The style-first candidate reduces matched device allocation by 1,144 B, but
first paint is 37.052 ms slower and worst observed core-0 headroom falls from
21.24 to 6.48 µs. All deadline counters remain zero. It is not promoted over
the 148-byte qualified firmware. See UI_MEMORY_OPTIMIZATION.md for the complete
comparison. The approximately 50-byte goal remains open.

## Previous qualified 148-byte accounting (explicit initialization affinity)

| Area                           | Bytes | Contents and reason retained                                       |
| ------------------------------ | ----: | ------------------------------------------------------------------ |
| Computed style                 |    80 | Resolved geometry and paint inputs described below                 |
| Tree links and payload handles |    18 | Five 16-bit links, rare-data handle, 4-byte text handle and tag ID |
| Layout                         |    38 | Live/previous boxes, inline/static positioning, two-entry memo     |
| Render state                   |    10 | Dirty categories, shared repaint scratch and inline baseline       |
| Type and final alignment       |     2 | One-byte node type and one-byte padding                            |
| Total                          |   148 | Associated allocations are additional and counted separately       |

The 80-byte style consists of:

| Fields                                                                      | Bytes |
| --------------------------------------------------------------------------- | ----: |
| display/explicit, flex direction/explicit, justification, alignment         |     6 |
| box sizing, automatic-margin mask                                           |     2 |
| inherited unitless line-height bits, width-expression handle                |     8 |
| pixel width/height and percentage width/height                              |     8 |
| minimum height, maximum width/height                                        |     6 |
| margin edges                                                                |     8 |
| absolute and percentage position edges                                      |     8 |
| background/active/text/border paint flags and alpha, position               |     6 |
| background/active/text/border colors                                        |     8 |
| font ID and weight                                                          |     4 |
| text alignment, uniform overflow, white space, text overflow                |     4 |
| border-color binding flags                                                  |     1 |
| gap, flex grow/shrink, padding, border width/radius, font size, line height |    11 |
| Total                                                                       |    80 |

Automatic range proofs now include the complete source graph, CSS scaling,
inheritance and native defaults. Unknown values retain wide fields. The pedal
qualifies for byte storage in all seven supported families; its largest radius
is 65 CSS pixels, or 130 physical pixels. Each newly narrowed field compiles to
one `l8ui`, replacing one `l16si`/`l16ui`. There is no extra load, sign extension,
per-read range check or decoding for these fields. The common-storage control
remains saved at 176 bytes per node and 100 bytes per computed style.

These are engine inputs, independent of whether application code calls a
computed-style API. Flex, border-box sizing, percentages, active button colors,
text inheritance, ellipsis and absolute positioning are used by this app.
Position storage now retains only the pixel top/right/left and percentage left
fields this app uses; the other four fields disappear automatically. Some flags
are still constant on subsets of nodes. They are not evidence of a 50-byte lower bound.
Further reduction needs a broader storage or whole-program proof strategy.

The 38-byte layout contains 8 B of live geometry, 8 B of previous geometry,
5 B of inline/static-position bookkeeping, 16 B of memo state and 1 B of padding.
The unused scroll family is automatically absent; apps that scroll retain all
six 32-bit fields, including the large-list regression case.
Removing memoization would restore repeated subtree measurement. Moving either
record into a full-capacity array would merely move bytes, not remove them.

The 10-byte render state contains 6 one-byte flags and a 4-byte union for the
original background color or partial-text bounds. The destination background
color is read directly from computed style. Mixed paint changes cancel the
shortcut; consecutive text changes union their dirty extents. Both scroll-dirty flags are absent
when scrolling is unused. Ordinary byte flags are grouped before aligned fields;
there is no bitfield decoding or extra pointer load.

The remaining 64-byte optional record contains attributes (12 B), listeners
(16 B), two custom-property vectors (24 B), two override pointers (8 B), and
inline static positioning (8 B, reusing 4 B of listener tail padding). The unused virtual-list pointer is absent. Its populated
vectors, strings and blocks are included in the allocation census.

The range audit deliberately retains signed margin edges: this app uses -1.5px
and -6px margins, and an S3 signed-byte read would need a separate sign-extension
instruction. Width-expression handles also retain their full range, including
pooled IDs and negative intrinsic-sizing tags. Minimum height reaches 410
physical pixels, font weight is 400/700, and the 600-pixel canvas requires more
than eight bits for geometry. No capacity or coordinate range is reduced to
make a sizeof result look smaller.

## Remaining architectural questions

The tree still reserves 160 stable Node slots. A moving vector would invalidate
native pointers and references; paging would change node lookup costs. Immutable
shared styles could remove duplicate style payloads, but would require explicit
mutation ownership and an extra style-base load. Neither change is implemented
or advertised as free. The rejected accessor proposal remains unapplied.

The deferred flex-basis handle is now automatically absent, leaving no rare-style
instance fields or pool allocation. The empty C++ type has sizeof 1; no such
record is embedded per node. A smaller main record still
requires broader proof about default fields and reachable node behavior; the
148-byte checkpoint is not a demonstrated lower bound. Immutable custom-property
strings are shared only in cold variable storage, while resolved style reads
remain embedded.

For the original consolidated batch, the host fixture saved another 7,520
allocated bytes relative to its preceding development build, or 76,096 B
versus registry engine 0.1.6,
in its 49-node attributes/listeners/styles phase. Both rounded and plain
variants match all six registry pixel snapshots. Host bytes are not S3 heap
bytes and must not be added to device measurements.

No packages were published. The development engine and elements artifacts are
explicit overrides; the pedal lockfile remains registry-only. Final package
publication and registry-only build qualification remain outstanding.

## Follow-up evidence

Repeating the unchanged 212-byte candidate reproduced the startup/USB concern:
first paint took 5,083,952 µs and the soak recorded 274 padding / 320 trimmed
frames. DSP misses and USB errors remained zero. The smaller memo is not yet
identified as the cause: a comparison against the saved preceding engine on
84 nested-flex cases produced identical geometry, cache hits and 4,544 layout
calls in both builds.

An isolated S3 architecture probe can fit a base record into 56 bytes, plus a
56-byte auxiliary record and a 108-byte style. It is not integrated, and neither
sharing nor ownership is implemented. Assembly adds one style-pointer load for
an eight-field operation; a standalone field also needs that extra load. Those
associated records cannot be omitted from total memory accounting. The user rejected treating that extra load as an acceptable default. The probe
remains isolated; hot fields stay embedded and directly readable.

## Combined follow-up: rare families and CSS variables

Analyzer v8 automatically omits unused side-border, background-layer and
line-height-expression storage. RareStyle falls from 52 to 4 bytes on the S3;
the only remaining field is the deferred flex-basis handle. The layout/render
border helpers read the embedded common width directly in this specialization.
Unknown values and opaque native mutation retain full support automatically.

Custom-property entries shrink from 48 to 32 bytes by reusing stable immutable
atom strings, with owned fallback after atom exhaustion. Their parsed color
and length caches remain direct fields. The main Node stays 212 bytes and
NodeRareData stays 72 bytes; these associated savings are reported separately.
The host populated-node fixture saves a further 4,112 allocated bytes (80,208
versus registry). The firmware saves 15,968 bytes; maintenance median free
PSRAM increases by 516 bytes. See UI_MEMORY_OPTIMIZATION.md for device limits,
including the two startup deadline misses in the first steady audio window.

## Automatic unused-scroll elimination

The app's only scrollable CSS selector was an unused `.scroll` rule. Removing
that dead source rule lets analyzer v9 prove scrolling absent automatically.
Overflow clipping remains supported. Scrollable/unknown CSS, native lists and
inputs, local background attachment, escaped/dynamic element factories and
opaque native UI use retain the complete implementation. No app switch is used.

The combined change removes six 32-bit fields, one scroll-dirty byte, redundant
child-extent walks and pre-layout offset snapshots. Grouping ordinary byte flags
also removes padding. The linked S3 layout is Node 184 B, LayoutBox 38 B,
RenderState 16 B, and TreeState 33,712 B (160 slots). ComputedStyle remains 108 B;
RareStyle 4 B and NodeRareData 72 B are unchanged. Direct hot-field probes each
still compile to one aligned target load.

Nine native configurations match twenty-two geometry/pixel frames, including
hidden/clip/visible overflow updates. Retained scrolling passes scrollIntoView
checks. The host allocator census remains 1,126,272 B in the populated fixture:
its total allocated bytes do not decrease further despite the smaller record.
Five matched S3 maintenance samples before/after OTA show 5,176 B more free PSRAM.
Firmware shrinks from 3,074,208 to 3,060,896 B. See UI_MEMORY_OPTIMIZATION.md and
the saved scroll-pruning measurements for device timing and qualification limits.

The unchanged 184-byte candidate's UI soak repeated first paint at 3,984,071 µs
after its initial 5,074,718 µs; the exact saved control reproduced 3,975,955 µs.
The repeat also recorded one startup DSP deadline miss (320,120 cycles against
320,000). Memory savings are verified, but startup timing and deadline misses
remain unexplained, so the candidate is not release-qualified. No additional
pointer load has been accepted as a tradeoff. Remaining cold scroll/list state
and deferred flex-basis storage warrant a combined audit; the 50-byte goal is
still open.

## Combined cold-storage follow-up

Analyzer v10 removes unused deferred flex bases and custom-property length
caches automatically. Relative bases, percentages, functions and unknown values
retain them. Literal colours and quoted strings need no length cache; an ambiguous
escaped quote conservatively retains support. Empty RareStyle storage and its
pool disappear, including with inline rare-style configuration. Removing its
2-byte handle becomes alignment padding in ComputedStyle: Node stays 184 B.

The S3 custom-property record is 24 B (previously 32 B), NodeRareData is 68 B
(previously 72 B), and TreeState is 33,680 B (previously 33,712 B). The latter
also drops unused dirty-scroll words and pending-scroll state. All sampled hot
fields still compile to the same one-load instructions. Eleven native variants
match 22 pixel/geometry frames, and the full flex-basis regression passes.

The host populated allocation census is unchanged at 1,126,272 B. Matched S3
maintenance free PSRAM rises from 4,036,608 to 4,104,168 B (+67,560 B); 65,536 B
of that comes from crossing a firmware mapping-reservation page boundary, not
per-node allocation. Firmware shrinks 10,608 B to 3,050,288 B. These are separate
measurements; the node target remains open. Device timing and release status are
recorded in UI_MEMORY_OPTIMIZATION.md.

## Combined common-field follow-up

The next batch reduces the S3 Node from 184 to 176 B and ComputedStyle from
108 to 100 B. It omits the unused scalar flex basis, gradient-only fill flag,
and duplicate overflow-axis fields. The existing overflow helpers read the
single scalar directly when the analyzer proves both axes always equal. Longhands,
unequal/unknown shorthand values and native inputs/lists retain separate axes.
Moving position into the byte flags closes the resulting alignment hole, while
the full-feature layout is preserved.

Maximum height is independently optional, but the pedal's chain UI uses it:
analyzer v11 correctly keeps that field. Unused transform scan state also
vanishes. TreeState is 32,384 B, saving 1,296 B; optional node data (68 B) and
custom-property entries (24 B) are unchanged. The firmware is 3,046,128 B,
4,160 B smaller. S3 overflow probes replace two loads and conditional arithmetic
with one byte load and sign extension. Other sampled hot fields retain one
aligned load. No extra pointer access or bitfield decoding is introduced.

Fourteen native configurations pass 22 geometry/pixel frames; the analyzer
passes 259 cases and the CLI passes 19. The host allocation census saves another
16,384 B, reaching 1,109,888 B in the populated fixture (96,592 B below registry),
with all six plain/rounded pixel phases matching. These host bytes must not be
added to S3 heap savings. Hardware results and qualification limits are kept in
UI_MEMORY_OPTIMIZATION.md.

The first incremental image was rejected: its ELF contained both 176-byte and
180-byte Node layouts and produced a black framebuffer despite mounting the
expected nodes. Its apparent 185,244 B free-heap gain is invalid. A clean native
rebuild passes the new linked-record layout check, and restores the control's
15-node, 35,699-pixel maintenance census. Five fresh samples measure 2,328 B more
free PSRAM and unchanged internal SRAM; firmware mapping and BSS are unchanged.
The focused rounded-root regression also paints 26,968 pixels on the host.

Remaining nonnegative scalar fields may admit automatic range specialization,
but only after proving bounds including CSS scaling, inheritance and native
defaults. No narrower range or packed representation has been assumed. The
approximately 50-byte goal still requires more than this unused-storage batch.

## Native payload follow-up

Whole-source `node-analysis-v1` automatically removes the two-byte image handle
when no image node can be created, plus native input/caret state and keyboard
code when input nodes are absent. Unknown imports, markup, factory aliases and
native writes retain support. The pedal's preset keyboard consists of ordinary
buttons and remains present. The no-scroll specialization also omits the
`non_scroll_dirty` flag and its writes. No accessor or additional field-read
computation is introduced.

The S3 Node falls from 164 to 160 B, RenderState from 16 to 14 B, and the
160-slot TreeState from 30,464 to 29,816 B. The payload-only build saves 11,664 B of firmware and static
PSRAM BSS saves 624 B; mapping reservation is unchanged. The 49-node host
allocator fixture remains unchanged at 1,109,888 B in its populated phase.
Width, color, font and memo assembly probes still contain one direct load each.
The approximately 50-byte goal remains unfulfilled; the remaining 88-byte
computed style is used by the layout and paint engine regardless of application
use of the computed-style API.

The follow-up pedal change keeps framebuffer and render-stack diagnostics in
maintenance mode only. It does not alter the UI record representation, audio
math or deadline counters. The payload-only image and its failed startup window
remain saved separately; the current report distinguishes that failure from
subsequent diagnostic-scoped measurements.

## Repaint scratch and rejected memo ownership experiment

The repaint batch shrinks Node 160 → 156 B, RenderState 14 → 10 B and fixed
TreeState 29,816 → 29,176 B on S3. No additional pointer loads or per-field
accessors are introduced: target probes still use one aligned load. The
mutation bookkeeping also fixes coalesced changes that previously left stale
pixels. Of 24 matched flat/rounded scenes, the old engine fails 16 and the new
engine fails none; two additional raw native color-write cases pass. Full and
exact-pedal configurations match presented pixels to a clean repaint, and
nested-flex geometry/cache work remains 4,544 layout calls across 84 cases.

The candidate firmware is 1,200 B larger due to the correct mutation handling.
This is a small RAM reduction plus a correctness fix, not a firmware-size or
speed improvement. The matched host allocation census still saves 96,592 B
against registry; no additional total-allocation saving is established there.

A separate attempt to delete the shared memo-result dimensions replaced them
with invalidation at geometry writes. It preserved geometry but raised layout
calls from 4,544 to 15,228 across the same 84 nested-flex cases. That experiment
was removed in full, including its size-write helpers and build flag. Its diff
and results remain under `build/css-feature-audit/rejected-compact-layout-memo.*`.
The retained candidate keeps both memo slots and their shared result snapshots.

## Remaining source-proof audit

The current pedal does use explicit active backgrounds (Presets, Pager,
PresetName, Amplifiers, Chain and App CSS), `margin-left: auto` (PresetName),
unitless line height (App), deferred widths (multiple screens), and dynamic
percentage widths (Slider). These fields cannot be removed merely because the
application does not call the computed-style API. A preliminary suggestion that
active-background storage was unused was incorrect; no such pruning was applied.

The class-token proof is now implemented. It selects two inline slots for the
pedal, reducing associated TreeState storage by 640 B while preserving overflow
support. Unknown imports, mutations, assertions and native constructors keep the
default four slots. The CLI derives the capacity; the app cannot opt into it.
All four capacities pass overflow/copy/move/reuse tests, and 22 geometry/pixel
frames match. This reduces associated storage, not the 148-byte Node itself.

The byte-sized maximum-height idea also needs its own range and sentinel proof;
opaque colors need proof through CSS variables before any alpha field can
become constant. These are possible analyses, not evidence that the fields are
unused or that their current bytes can simply be deleted.
