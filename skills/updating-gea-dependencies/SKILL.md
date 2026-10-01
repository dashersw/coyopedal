---
name: updating-gea-dependencies
description: Use when bumping any @geastack or @geajs package, when the pedal needs a fix that lives inside a Gea package, or when a board-specific need has to be expressed to Gea rather than patched into it.
---

# Gea dependencies

Install with `npm ci`, which installs exactly what `package-lock.json` names.
Core, engine and host are pinned in `package.json`; other dependencies track `latest`.
The committed lockfile records the exact package set used for a build, with no
local patches. What this pedal needs from Gea it asks for, and the rest is fixed
upstream.

## Bumping a package

1. Run `npm install` (or `npm install --save-exact <package>@<version>` for a pinned
   dependency). It
   rewrites `package-lock.json`; commit that, since it is the record of what
   the firmware was built against.
   Check the transitive version changes, then run `npm ci` to verify the lockfile
   installs cleanly. Every package must resolve from the registry into this
   project's `node_modules`, with no `file:`/`link:` dependencies or symlinks to
   local Gea checkouts.
2. Run `npm test`, `npm run check` and `npm run build:firmware`.
   Clear package-path/compiler overrides (`GEA_TARGETS_ROOT`, `GEA_*_DIR`,
   `GEA_GEATSC_BIN`, `GEATSC*`) before qualifying the build. Preserve the old
   target build directory outside `.gea/` and start a fresh cache under the
   normal `.gea/build/` path when switching from a local target checkout. Then
   inspect the generated CMake cache and compile inputs for paths outside the
   project, its installed packages and the ESP-IDF/toolchain. Run the web/WASM
   build too when shared Gea dependencies change.
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
