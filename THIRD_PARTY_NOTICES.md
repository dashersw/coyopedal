# Third-party notices

## NeuralAmpModelerCore

The A2 model shape, weight order and inference equations used by the engine in
`src/audio/nam/` are derived from
[NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore).

```text
MIT License

Copyright (c) 2023 Steven Atkinson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## VoLum amp captures

`assets/models/volum-herbert-1-v30.namb` (Diezel Herbert, channel 1, V30) and
`assets/models/volum-ampete-4-v30.namb` (Ampete One, channel 4, V30), and
`assets/models/volum-ampete-4-amp.namb` (Ampete One, channel 4, amp only) are
converted without changes to their weights from `V30-Herb-1.nam`,
`V30-Ampt-4.nam`, and `AMP-Ampt-4.nam` in [VoLum](https://github.com/guitarlum/VoLum). The amp
profiles are by Lum. `tools/fetch_volum.py` downloads the rest of VoLum's
captures onto an SD card, together with this notice; they are not part of this
repository.

```text
MIT License

Copyright (c) 2024-2026 Steffen Dangmann and VoLum contributors
Portions copyright (c) 2022-2025 Steven Atkinson and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The amp-only capture comes from VoLum commit
`ddb7822ce06df9eaa74156027cf175a9ffe8305a`, file
`rigs/Ampete One/AMP-Ampt-4.nam`. Its original SHA256 is
`0044c191e60bd206a084b637bf0fd4a51ac28cde7381ce9aa821ed131ffdbde9`.

## Jester Dyne cabinet IRs

`assets/cabinets/` contains three 48 kHz, mono, PCM24 cabinet responses by
Bastian Karschewski (Jester Dyne Productions), from
[Jester's Brutal Pack 1.0](https://www.jester-dyne-productions.com/brutal-ir-pack/).
The original handbook explicitly dedicates these IRs to the public domain under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/), dated 2022.

| Asset                        | Original 48 kHz file    | Speaker / microphone                          |
| ---------------------------- | ----------------------- | --------------------------------------------- |
| `jester-v30-sm57.wav`        | `1_Cookie_Monster.wav`  | Celestion Vintage 30 / Shure SM57             |
| `jester-dv77-sm57.wav`       | `2_Darth_Genocider.wav` | Eminence DV-77 / Shure SM57                   |
| `jester-rockdriver-e606.wav` | `3_Kitten_Slayer.wav`   | Celestion Rockdriver Junior / Sennheiser e606 |

All three use the creator's modified Behringer BG412S cabinet. The files keep
the original first 1,024 samples without normalization, resampling or filtering;
only the remaining tail and ancillary WAV chunks are removed. The source
[archive](https://www.jester-dyne-productions.com/content/files/2023/04/JestersBrutalPack_1.0.zip)
has SHA256 `299dc053f01ebd1e980459adc48f9c6b8a8c7af91917b4f946512eefdbb311ea`.
See `assets/cabinets/LICENSE.txt`.

## ESP-IDF USB Host library

`third_party/usb/` is Espressif's `usb` component, version 1.5.0, with local
changes for isochronous audio transfers. It is licensed under the Apache License
2.0; the full text is in `third_party/usb/LICENSE`.

## ESP-DSP cabinet FFT

`src/audio/cabinet_fft64.S` adapts the 64-point radix-four transform from
[ESP-DSP](https://github.com/espressif/esp-dsp/blob/master/modules/fft/float/dsps_fft4r_fc32_aes3_.S),
Copyright 2018–2025 Espressif Systems (Shanghai) CO LTD, with contributions
from f4lc0n (2024). It uses caller-owned tables, fixed transform size, inverse
support and IRAM placement. It is licensed under Apache-2.0; the full text is
in `third_party/usb/LICENSE`.

## cJSON

`third_party/cjson/` is cJSON, Copyright (c) 2009-2017 Dave Gamble and cJSON
contributors, licensed under the MIT License. The full text is in
`third_party/cjson/LICENSE`.

## Saira

`assets/fonts/Saira-Bold-Pedal.otf` is derived from a static weight-700
instance of the Saira variable font, Copyright 2020 The Saira Project Authors
(https://github.com/Omnibus-Type/Saira), licensed under the SIL Open Font
License 1.1. The full text is in `assets/fonts/OFL.txt`.
It is renamed Saira Pedal, cut down to the characters the panel can draw,
and carries the UI's own glyphs, drawn in by `tools/font_glyphs.mjs`; it is
under the same license.
