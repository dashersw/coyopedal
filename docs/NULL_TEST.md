# Null-testing a device against the original capture

A null test answers one question about a pedal that claims to run a NAM
capture: is the sound the capture, or a re-trained approximation of it? The
original `.nam` is rendered on the host with the official
[NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) in
float, the same DI is played through the device and recorded, and the two are
aligned, gain-matched and subtracted. What is left is the difference, and its
level relative to the reference is the **null depth**.

`tools/null_test.py` does the host side. This page is the procedure for a
device that only takes the DI as analog audio, written for the MOOER Prime S1
and its "A2 conversion", but any pedal works the same way.

## Setup

```bash
bash scripts/null-test-setup.sh ~/Downloads/guitar_techs_di_11-5150-andy-sneap-od808-full-rig-a2-nano.wav
```

This builds the renderer, makes a Python venv, downloads the three original
VoLum captures and stages the DI, all under `build/`. The default captures are
the two factory amps and a Marshall 2204:

| Pedal id              | VoLum file                               | Tone name         |
| --------------------- | ---------------------------------------- | ----------------- |
| `volum-ampete-4-v30`  | `rigs/Ampete One/V30-Ampt-4.nam`         | Ampete One C4 V30 |
| `volum-herbert-1-v30` | `rigs/Diezel Herbert Mk1/V30-Herb-1.nam` | Diezel Herbert C1 |
| —                     | `rigs/Marshall 2204 1982/V30-2204-1.nam` | Marshall 2204 V30 |

Pass other `"<amp>/<capture>"` paths to the script to test different ones. The
DI is the 15 s guitar take at 48 kHz that the earlier RA8M2 comparison used;
any mono 48 kHz guitar DI works, but use the same file for every recording.

The `.nam` files are SlimmableContainers with both members: `--slim 1.0` renders
A2-Full (eight channels), `--slim 0.0` renders A2-Lite (three channels). The
reference is always A2-Full, which is what the pedal runs and what the file
sounds like in the plugin.

## Recording the Prime S1

The Prime S1 shows up as a 2-in, 2-out USB audio device that runs at 48 kHz.
Its USB **output** carries the processed signal, which is the cleanest thing to
record. Its USB **input** does not reach the effects chain (playing the DI into
it records silence), so the DI has to go in as analog: interface line out →
¼" cable → Prime S1 guitar input. On this desk that is the iRig PRO DUO's
outputs, both channels carrying the DI.

`tools/null_test_record.py` plays the DI and records in one go:

```bash
build/null-test-venv/bin/python tools/null_test_record.py \
  --di build/null-test/di.wav --play "iRig PRO DUO" --play-channel -1 \
  --record "Prime S1" --out build/null-test/recordings/prime-s1-herb-1.wav
```

Without arguments it lists the audio devices. Keep every level in the chain
where it is for all recordings.

1. Convert the captures in MOOER Studio for S1 (v2.0.0 adds NAM A2 loading)
   and load each onto its own patch.
2. On each patch switch off everything but the amp block: **no cab or IR** (the
   VoLum captures are full-rig captures with the cabinet in them), no gate, no
   EQ, no effects. With the cab on the Herbert null was 4.6 dB; off, 6.6 dB.
3. Record the DI through an **empty patch** too. That bypass recording gives
   the tool the round-trip latency and, more importantly, the level at which the
   DI reached the pedal. A NAM model is level-dependent: a DI that arrives 11 dB
   lower drives the amp differently, and that is a different sound, not a
   quieter one. Pass it as `--bypass`.
4. Without a bypass recording, `--sweep -18:6:2` renders the reference at each
   DI level in that range and picks the one that nulls best. On this desk the
   pedal saw the DI about 11 dB below the file, so the sweep is what found the
   level.

## Recording the Prime S1 digitally (no analog leg)

The analog leg limits the floor (the empty patch nulls 19/27 dB). The
`--di-inject` firmware image in `taurus-pedal/docs/mooer-prime-s1` removes it:
the pedal boots its stock chain, USB playback L replaces the guitar ADC, and
the chain output is what the USB input carries. Everything below runs from
that folder; it needs no clicking in MOOER Studio.

1. Convert the captures with the app's own converter (it is a plain CLI):

   ```bash
   /Applications/MooerStudioForS1.app/Contents/Resources/nam/nam_to_gnr_cli \
     --input build/null-test/models/V30-Herb-1.nam --output build/null-test/models/V30-Herb-1.gnr
   ```

   A `.gnr` is 10324 bytes: a header and a 10240-byte `data` chunk that is
   exactly one user amp slot on the pedal. The converter's strings call the
   model "WH": Mooer fits a Wiener-Hammerstein model to the capture, not a
   network.

2. Build the image with up to two conversions embedded (the reclaimed flash
   holds about 25 KB), verify, pin the hash and flash:

   ```bash
   python3 build_interface_firmware.py .work/vendor/MOOER_S1_V1.4.2.mr .work/midi-config .work/di-inject \
     --di-inject --gnr .../V30-Herb-1.gnr --gnr .../V30-Ampt-4.gnr
   python3 verify_di_inject.py .work/di-inject
   # pin manifest output_sha256 into DI_INJECT_EXPERIMENT_SHA256 in s1_usb.py, then
   python3 s1_usb.py --bridge .work/s1-usb-bridge --log .work/di-inject/a.log enter-update
   sleep 8
   python3 s1_usb.py --bridge .work/s1-usb-bridge --log .work/di-inject/b.log \
     install-di-inject-experiment --file .work/di-inject/S1-V1.4.2-interface.mr
   sleep 20
   ```

   Every firmware install resets the 40 presets to factory, so nothing loaded
   from MOOER Studio survives a flash; the embedded models are the way in.

3. Pick the chain from the host. `--model 100` is the first embedded
   conversion, `101` the second; `--blocks amp` leaves only the amp slot
   running (the image rewrites every other slot to its passthrough id before
   each DSP block, because block enables edited in the preset do not stick);
   `--knobs-neutral` puts the six amp knobs at 50:

   ```bash
   python3 di_select_preset.py --bridge .work/s1-usb-bridge --log .work/di-inject/sel.log \
     --preset 0 --blocks amp --knobs-neutral --model 100
   ```

   Never run it while an audio stream to the pedal is open (the HID side
   wedges until a power cycle).

4. Record and compare at the pedal's own rate. The chain runs at 44.1 kHz;
   a 48 kHz stream goes through CoreAudio's resampler, so play the 44.1 kHz
   DI (`build/null-test/di-44100.wav`, the 48 kHz DI through a Kaiser
   polyphase filter) and give the null tool `--rate 44100`. It still renders
   the reference at 48 kHz and resamples it with the same filter. The amp
   block takes about 15 s after a stream starts to settle (two takes differ
   by 10 dB for that long, then agree bit for bit), so play 16 s of silence
   and drop 14 of it:

   ```bash
   build/null-test-venv/bin/python tools/null_test_record.py --di build/null-test/di-44100.wav \
     --play "Prime S1" --record "Prime S1" --lead 16 --discard 14 \
     --out build/null-test/recordings/prime-s1-44k-herb.wav
   build/null-test-venv/bin/python tools/null_test.py --rate 44100 \
     --model build/null-test/models/V30-Herb-1.nam --di build/null-test/di.wav \
     --dut build/null-test/recordings/prime-s1-44k-herb.wav \
     --sweep -10:6:1 --max-lag 3 --out build/null-test/out --label herb-44k
   ```

   The empty chain is recorded the same way with `--blocks off` and nulled
   with `--rate 44100 --reference build/null-test/di-44100.wav`.

5. The chain has a stage ahead of slot 0 that no preset can switch off: a
   one-pole smoother on the mono input whose coefficient follows the tracked
   input level (0x59b4). With it the empty chain nulls only 26 dB. The image
   replaces it with a straight copy while injecting; `di_select_preset.py
--input-stage stock` puts it back, `--input-stage bypass` (the default
   after boot) takes it out again.

Two things to know about the pedal's chain when reading its state pages: slot
type 4/5 is the amp (its function runs the nonlinear engine when the loader
flags a user model), type 7 is the cabinet (a 16-sample FIR exchange plus two
biquads). Preset 0 as shipped is "Dumb Clean", a linear model.

## Running the comparison

```bash
build/null-test-venv/bin/python tools/null_test.py \
  --model build/null-test/models/V30-Ampt-4.nam --di build/null-test/di.wav \
  --bypass recordings/prime-s1-bypass.wav --dut recordings/prime-s1-ampete-4.wav \
  --out build/null-test/out --label ampete-4-prime-s1
```

Or, with no bypass recording, `--sweep -18:6:2` in place of `--bypass`.

Once per amp, same `--bypass` each time. The report gives the latency, the
polarity, the gain it applied, the overall null depth, the worst 100 ms window,
the depth per octave band, then the coherence and the DUT/reference transfer
function per band and the null that remains after that linear response is
fitted onto the reference. A low null with high coherence and a sloped transfer
function is an EQ, a cab or a low-pass still in the chain; a low null with low
coherence is a different nonlinearity, that is, a different model. The aligned
reference, the aligned recording and the residual are written as float WAVs to
listen to. Two renders can also be
compared directly with `--reference` in place of `--model` and `--di`.

## Reading the numbers

Measured with this tool on the same DI:

| Comparison                                                        | Null depth                                |
| ----------------------------------------------------------------- | ----------------------------------------- |
| Two float renders of the same A2-Full model                       | > 300 dB                                  |
| A2-Full delayed 137.3 samples, -3 dB, inverted, -60 dB noise      | 57 dB                                     |
| A2-Lite vs A2-Full, Ampete One C4 V30                             | 11 dB                                     |
| A2-Lite vs A2-Full, Diezel Herbert C1 V30                         | 21 dB                                     |
| A2-Lite vs A2-Full, Marshall 2204 V30                             | 21 dB                                     |
| The recording chain itself: iRig DAC → Prime S1 empty patch → USB | 19 dB (27 dB after fitting its low-pass)  |
| Prime S1 "A2 conversion" of Herbert C1 V30 vs A2-Full, cab off    | 9 dB (11 dB after fitting its EQ)         |
| Digital, 48 kHz via CoreAudio, stock input smoother, empty chain  | 26.6 dB (36.1 dB after fitting)           |
| Digital, native 44.1 kHz, input smoother bypassed, empty chain    | 108.4 dB                                  |
| Digital 44.1k, Herbert C1 V30 conversion, amp only, knobs 50      | 10.6 dB (13.0 dB after fitting), r = 0.88 |
| Digital 44.1k, same with the stock input smoother                 | 9.8 dB (12.2 dB after fitting), r = 0.86  |
| Digital 44.1k, Marshall 2204 V30 conversion, amp only, knobs 50   | 11.6 dB (12.6 dB after fitting), r = 0.88 |
| Digital 44.1k, Ampete One C4 V30 conversion, amp only, knobs 50   | 2.3 dB (3.4 dB after fitting), r = 0.60   |
| Digital 44.1k, Herbert conversion, gain knob 100                  | 8.3 dB (9.4 dB after fitting)             |
| Digital 44.1k, Herbert conversion, gain 0 (reference at -30 dB)   | 16.0 dB (32.6 dB after fitting), r = 0.91 |

The chain on this desk is not transparent: the empty patch rolls off from
2.5 kHz (-3 dB at 2.5-5 kHz, -8 dB at 5-10 kHz, -21 dB above 10 kHz), and the
iRig and the pedal run on different crystals, about 4 ppm apart, which the tool
measures and resamples out before aligning (without that the chain nulled at
15 dB). The floor is therefore the linear-corrected chain figure, 27 dB; a
result within a few dB of it would be the capture. The pedal's amp block also
wants the DI about 8 dB lower than the empty patch says it received it: the
level sweep peaks near -10 dB while the bypass measures -2 dB, so the
conversion or the amp block scales its input.

The digital rows are the clean answer. Played natively at 44.1 kHz with the
input smoother taken out, the empty chain nulls 108 dB: the path is
bit-transparent to the level of float rounding, zero drift, lag 0. The 26 dB
of the first digital floor was the smoother, a level-dependent low-pass that
no preset controls. Against that floor every conversion is plainly a
different model: the Herbert and the 2204 land at 11 to 12 dB with
correlation 0.88, the Ampete at 2 dB with correlation 0.6 whatever level the
reference is rendered at. The gain knob is an input trim on the conversion:
at 0 the pedal behaves like the capture driven 30 dB softer and nulls 16 dB,
33 dB after the linear fit, so the conversion's linear response matches the
capture and the divergence is in the nonlinearity; at 100 it nulls 8 dB.
Putting the stock smoother back costs under a dB on the model rows, so it
does not change the verdict. On a 1 kHz tone the Herbert conversion starts
to saturate near -30 dBFS and reaches -5 dB THD by -6 dBFS, where the
capture is already at -0.8 dB THD from -12 dBFS.

Takes recorded less than about 15 s after a stream starts are not
repeatable (the amp block is still settling; two such takes null each other
only 15 dB, then agree bit for bit), and they null the capture 2 to 5 dB
worse. Every 44.1 kHz row above is a settled take.

So the scale is: the bypass recording sets the floor the chain can reach
(a clean interface loop lands at 40 to 60 dB); a pedal that runs the capture
lands within a few dB of that floor; and a pedal that re-trained a smaller
network on the capture's output lands where A2-Lite does, 10 to 20 dB, with
the loss concentrated in the highest octaves and the lowest. Anything under
10 dB is a different amp.
