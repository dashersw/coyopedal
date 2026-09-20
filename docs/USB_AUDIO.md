# USB audio

The pedal has no audio converters of its own. The board's USB-C port runs as a
USB host, and a class-compliant USB audio interface provides the guitar input
and the output. This document covers how the interface is discovered, which
formats work, how transfers are scheduled and what the transport cannot do.
The DSP side is described in [ARCHITECTURE.md](ARCHITECTURE.md).

The transport is in `src/native/drivers/usb_audio.cpp`. Descriptor discovery and
sample conversion are in `src/native/drivers/uac2.hpp` and `uac2_pcm.hpp`, which
have no device identities, no OS dependencies and no allocation at streaming
time.

## Discovery

USB Audio Class 2 interfaces are found from their descriptors. There is no
vendor or product allowlist. The only device-specific path is the iRig HD 2,
which predates UAC2 support and is driven through a fixed UAC1 layout (below).

When a device enumerates, the active configuration descriptor is parsed:

1. **Topology.** AudioControl functions, their input and output terminals,
   clock sources, clock selectors, clock multipliers and feature units, and the
   AudioStreaming alternates with their format, data endpoint and optional
   feedback endpoint. Malformed or truncated descriptors, duplicate entity IDs
   and streams that do not link to a USB streaming terminal are rejected with a
   specific error.
2. **Pairs.** Every capture stream is paired with every playback stream on a
   different interface of the same AudioControl function. Pairs are ranked:
   shared clock first, then the shortest service intervals, then the fewest
   channels, then 4-byte sample containers.
3. **Transport fit.** A pair is kept only if both endpoints use a 1 ms
   interval, each packet can carry at least 48 frames, capture carries no more
   than 96 frames per packet, and both packet sizes fit the host controller's
   FIFO split. A feedback endpoint must use a 1-4 ms interval and a 3- or
   4-byte value.
4. **Clock.** Both directions' clock paths are traversed, reading the current
   selector input and multiplying out any multipliers. Both must resolve to
   the same source with the same ratio. The source is then set to 48 kHz (after
   checking its advertised ranges when the rate differs), read back, and
   checked for validity when the source reports that control.
5. **Controls.** Feature units on the selected function are unmuted and set to
   0 dB on the master, left and right channels. Some interfaces enumerate muted
   or at minimum gain.

The first pair that passes every step is used. The log records the topology,
each candidate and the reason a candidate was skipped. If nothing qualifies,
the raw descriptors are logged so the case can be diagnosed from maintenance
mode.

## Supported formats

| Encoding                    | Container          | Notes                                   |
| --------------------------- | ------------------ | --------------------------------------- |
| Signed integer PCM (Type I) | 1, 2, 3 or 4 bytes | Any valid bit depth up to the container |
| Unsigned PCM                | 1 byte, 8 bits     |                                         |
| IEEE float                  | 4 bytes, 32 bits   | Non-finite input samples read as zero   |

- **Capture.** Channel 0 is the guitar input. Other capture channels are
  ignored by the signal path.
- **Playback.** Left and right go to the first two channels. A mono output
  receives the average of left and right. Channels beyond the second are
  silent.
- Signed PCM in 3- and 4-byte containers is decoded and encoded in dedicated
  loops; other layouts use per-sample converters. No layout allocates in the
  transfer callbacks.

## Tested interfaces

| Interface    | Class | Format at Full Speed                             | Playback pacing  |
| ------------ | ----- | ------------------------------------------------ | ---------------- |
| XTONE Pro    | UAC2  | Stereo in and out, 24-bit in 3-byte slots        | Capture packets  |
| IK iRig HD X | UAC2  | Descriptor-driven, as for any UAC2 device        | From descriptors |
| IK iRig HD 2 | UAC1  | Mono 24-bit in, stereo 16-bit out (fixed layout) | USB frames       |

The XTONE Pro routes both directions through a clock selector to a single
source and has no feedback endpoint, so playback follows the capture packet
sizes. It also offers a 16-bit playback alternate. Its Full Speed descriptors
differ from the ones it reports to a High Speed host (which use 4-byte slots),
so only the Full Speed capture is representative of what the pedal sees.

The iRig HD 2 is recognised by its vendor and product ID and uses fixed
interfaces and endpoints, with the sample rate set on each endpoint in the
UAC1 way.

Any other class-compliant interface that offers a 48 kHz duplex pair meeting
the requirements above should work. Reports of interfaces that do or do not
work are welcome.

## Transfer scheduling

The USB host library and the client task run on core 0. The client runs at
priority 20, above DSP stage A, and its completion callbacks are in IRAM.
Isochronous completions have to be handled on time: if stage A delays them,
playback transfers are resubmitted after their slot, the transfer fails and the
output has a gap.

| Endpoint | Transfers queued | Packets per transfer |
| -------- | ---------------- | -------------------- |
| Capture  | 3                | 1                    |
| Playback | 2                | 1                    |
| Feedback | 2                | 4                    |

- **Capture** decodes each packet into the input ring (2,048 frames, in PSRAM).
  Stage A takes 64-frame blocks from it. The ring is held near one block plus
  one packet, and a backlog of more than two further blocks is discarded in one
  step: capture and playback share one clock, so a backlog would otherwise
  persist as added latency.
- **Playback** fills each packet from the output ring (512 frames, static in
  internal SRAM).
  For UAC2 the ring is held near two packets plus one block.
- **Pacing.** With an explicit feedback endpoint, the most recent valid value
  (10.14 format at Full Speed) sets the size of the following packets. Without
  one, asynchronous playback replays the frame counts of the capture packets.
  Otherwise playback follows the USB frame cadence of 48 frames per
  millisecond.
- **Start-up.** Both streaming interfaces are first set to alternate 0 and then
  to their operating alternates. Some interfaces keep their previous
  alternate across a soft reset of the host and stop accepting audio unless the
  change is forced. UAC2 playback starts with silence before capture, because
  some interfaces produce no input until OUT packets arrive.
- **FIFO.** The controller's FIFO is biased toward periodic OUT
  (`CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT`), and the resulting input and
  output limits feed the transport-fit check.
- **Hubs** are supported (`CONFIG_USB_HOST_HUBS_SUPPORTED`).
- **Hot-plug.** When the adopted interface disconnects, callbacks stop
  resubmitting, outstanding transfers drain and the device is released. A new
  interface plugged in afterwards is discovered from scratch.

## Known limits

- **Full Speed only.** The ESP32-S3's controller is USB 1.1. Every interface
  runs with 1 ms packets, which adds about a packet of buffering on each side
  compared with a High Speed host.
- **48 kHz only.** Interfaces that cannot run at 48 kHz are refused rather
  than resampled.
- **One clock.** Capture and playback must resolve to the same clock. There is
  no asynchronous sample-rate converter.
- **Duplex only.** Input-only and output-only devices are not used.
- **Packet sizes.** Endpoints must use 1 ms intervals, carry at least 48
  frames, and fit the controller's FIFO. Capture packets are limited to 96
  frames.
- **Descriptor size.** Configurations up to 4,096 bytes are parsed, but the
  host stack's enumeration buffer (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`)
  is 1,024 bytes.
- **Channels.** Only the first capture channel and the first two playback
  channels carry audio.

## Tests and diagnostics

`npm test` runs the discovery and conversion code against recorded
configuration descriptors (`tests/usb/`) under AddressSanitizer and
UndefinedBehaviorSanitizer. The tests cover the iRig HD X and XTONE Pro
topologies (including the XTONE's Full Speed configuration), renumbered entity
IDs, truncated and malformed lengths, clock traversal, rate negotiation, packet
capacity checks and channel mapping. They do not show that a device streams
cleanly.

On the device:

- The `audio:` heartbeat, logged every five seconds in audio mode, reports
  captured, played and silence-padded frames, ring depths and transfer errors.
- The first four capture completions log the requested and actual packet
  sizes, the packet status and the controller's raw isochronous error.
- `STATUS` and the `audio window usb:` line of an `AUDIO TRY` window report
  the capture, playback and feedback callback costs.

These check what the CPU submits and when, not what the interface's converter
plays. [MEMORY.md](MEMORY.md) describes how to reach the logs.
