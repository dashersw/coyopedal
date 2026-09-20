# Notice

CoyoPedal
Copyright (C) 2026 Armagan Amcalar and contributors

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

## Components under other licenses

These parts of the repository are not covered by the GPL notice above. Each
keeps its own license, and those licenses are compatible with distributing the
firmware under the GPL.

| Component                           | Location                     | License    |
| ----------------------------------- | ---------------------------- | ---------- |
| ESP-IDF USB Host library            | `third_party/usb/`           | Apache-2.0 |
| cJSON                               | `third_party/cjson/`         | MIT        |
| Saira typeface                      | `assets/fonts/`              | OFL-1.1    |
| VoLum amp captures                  | `assets/models/volum-*.namb` | MIT        |
| NeuralAmpModelerCore (derived work) | `src/audio/nam/`             | MIT        |

The license texts and copyright notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and in the component
directories.

## Additional permission for the Espressif binary components

ESP-IDF ships the Wi-Fi, Bluetooth and radio PHY stacks as precompiled
libraries with no source, and the firmware links them. As the copyright holder
of this program I give permission to link it with those components and to
convey the resulting work; this is an additional permission under section 7 of
the GPL, it covers nothing else the program links, and you may remove it from a
copy you pass on.

The Gea toolchain and runtime (`@geastack/*` and `@geajs/*`) are npm
dependencies. They are not part of this repository, they are used unmodified,
and their own licenses apply.
