# Agent skills

Task-focused instructions for AI agents working in this repository. Each folder holds one
`SKILL.md` with `name`/`description` frontmatter, following the Agent Skills format.
`AGENTS.md` stays the always-loaded summary; these skills hold the detail an agent needs only
for a particular task.

| Skill                                                                             | Use it for                                                       |
| --------------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| [building-pedal-firmware](building-pedal-firmware/SKILL.md)                       | Building, flashing or OTA-updating the ESP32-S3 firmware         |
| [running-host-checks](running-host-checks/SKILL.md)                               | Tests, lint, type checks and formatting on the host              |
| [measuring-pedal-audio](measuring-pedal-audio/SKILL.md)                           | Reaching the board over Wi-Fi and diagnosing silence or crackles |
| [changing-sram-and-code-placement](changing-sram-and-code-placement/SKILL.md)     | Memory placement, IRAM, linker fragments, sdkconfig, core split  |
| [editing-the-gea-ui](editing-the-gea-ui/SKILL.md)                                 | Screens, stores, styles, the board boundary and the web preview  |
| [editing-factory-models-and-presets](editing-factory-models-and-presets/SKILL.md) | Factory amps, the capture library and starter presets            |
| [updating-gea-dependencies](updating-gea-dependencies/SKILL.md)                   | Bumping `@geastack` packages and checking what a version moved   |

Keep a skill short and current. When a command, path or default changes, update the skill in
the same change; a stale skill misleads more than a missing one.
