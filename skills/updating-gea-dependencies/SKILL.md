---
name: updating-gea-dependencies
description: Use when bumping any @geastack or @geajs package, when the pedal needs a fix that lives inside a Gea package, or when a board-specific need has to be expressed to Gea rather than patched into it.
---

# Gea dependencies

Install with `npm ci`, which installs exactly what `package-lock.json` names.
Every dependency asks for `latest` in `package.json`, so the lockfile is the
only thing that decides a version: Gea's releases and the pedal move together,
and there are no local patches. What this pedal needs from Gea it asks for,
and the rest is fixed upstream.

## Bumping a package

1. Run `npm install` (or `npm install <package>@latest` for one of them). It
   rewrites `package-lock.json`; commit that, since it is the record of what
   the firmware was built against.
2. Run `npm test`, `npm run check` and `npm run build:firmware`.
3. Diff the generated `.gea/build/<target>/app-builds/<app>/sdkconfig` against
   `src/native/sdkconfig.defaults`. The CLI decides some settings itself and a
   new version can decide them differently; the generated file is sticky, so a
   defaults change needs a fresh configure. A 64 KiB data cache instead of
   32 KiB is what cost this board the internal SRAM its amp graph needs.
4. Check the device. A bump moves memory placement and code generation, and
   only the board shows that (`measuring-pedal-audio`,
   `changing-sram-and-code-placement`).

`@geastack/compiler` decides the generated native code, so treat its bump as a
device-timing change.

## Changing Gea itself

Work in the Gea checkout, not here. Two rules keep this pedal's limits out of
everyone else's build:

- **A defect is fixed unconditionally**, in the generic code.
- **A choice this board makes is a build switch**, off by default, set from
  `gea.defines` in `package.json`. The ones this pedal turns on are listed in
  the targets repository, `docs/BOARD-WORKFLOW.md`, "Internal RAM Switches":
  the engine's state and the render, frame and touch task stacks move to
  PSRAM, the gradient tables are allocated on first use, and the compiler
  runtime drops its allocation pool.

If something seems to need a patch, it is one of those two things instead.
