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
`assets/models/volum-ampete-4-v30.namb` (Ampete One, channel 4, V30) are
converted without changes to their weights from `V30-Herb-1.nam` and
`V30-Ampt-4.nam` in [VoLum](https://github.com/guitarlum/VoLum). The amp
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

## ESP-IDF USB Host library

`third_party/usb/` is Espressif's `usb` component, version 1.5.0, with local
changes for isochronous audio transfers. It is licensed under the Apache License
2.0; the full text is in `third_party/usb/LICENSE`.

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
