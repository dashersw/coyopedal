# UI

The touchscreen app is written once, in TSX, and runs in two places. On the
pedal, the Gea compiler turns it into native C++ that is linked with Gea's
layout, rendering and touch engine; there is no JavaScript engine on the device.
In the browser, Vite builds it against the Gea web runtime for the preview.

## Structure

- `index.tsx` sets the display pacing (15 fps, no vsync) and calls `mount(App)`.
- `components/App.tsx` picks the current screen. Each component module owns its
  JSX and imports a stylesheet with the same name. `components/App.css` holds
  only the shared shell, base styles and layout utilities.
- `stores/PedalboardStore.ts` extends Gea's `Store`. It owns navigation, preset
  actions and the view of the pedal's state. `stores/PageStore.ts` pages lists:
  no screen scrolls, and lists show a few rows at a time behind ‹ › buttons.
- `board.ts` is the typed boundary to the audio controls and preset storage.
  On the device it is backed by `src/native/ui/board_bridge.cpp`. In the browser,
  `src/preview/main.ts` installs a simulated pedal from
  `src/preview/board-fixture.mjs` before mounting the same components.

The logical layout is 251 × 205 points, drawn at 2× on the 502 × 410 panel with
its 130-pixel corner radius. The UI font is `assets/fonts/Saira-Bold-Pedal.otf`: Saira Bold with the UI's
icons drawn in as glyphs by `tools/font_glyphs.mjs`.

## Commands

```bash
npm run dev
```

```bash
npm run preview:panel
```

```bash
npm run check:frontend
```

`npm run dev` starts the Vite preview. `npm run preview:panel` builds it into
`build/ui-preview/`. `npm run check:frontend` type-checks the app and the
browser adapter. The store tests run with `node --test tests/gea-store.test.mjs`.

## On the device

`npm run build:firmware` compiles this entry point with the pinned
`@geastack/compiler`, loading `scripts/panel-host-plugin.mjs` (declared in
`gea.compilerPlugins`) to describe the board functions. Compilation fails
instead of falling back to dynamic code. The generated C++ and rasterized fonts
are written under `.gea/build/` and are never edited by hand.

`@geastack/targets` provides the panel driver, framebuffer and touch. The
pedal's own native side is small:

- `src/native/ui/board_bridge.cpp` implements the audio, model, preset and
  maintenance calls behind `board.ts`.
- `src/native/ui/controls.c` holds the audio control state.
- `src/native/ui/task.cpp` runs a low-priority pump for the tuner and
  requests repaints.

`src/native/gea_memory.lf` moves the framework's zero-initialized state and
render caches to PSRAM, and `gea.defines` shrinks Gea's text, CSS and transform
caches. Internal SRAM is reserved for the NAM engine; see
[docs/MEMORY.md](../../docs/MEMORY.md).

## Gea packages

The `@geastack/*` packages track `latest` and carry no local patches; the
lockfile records the versions a build used (see
`skills/updating-gea-dependencies`). What this board needs from
Gea it asks for in `gea.defines`: the `GEA_EMBEDDED_*_EXTERNAL` switches move
the engine's state and the render, frame and touch task stacks to PSRAM, and
`GEA_EMBEDDED_LAZY_GRADIENT_LUTS` and `GEA_RUNTIME_COMPACT_ALLOCATION` trim
allocations. Anything that is a defect rather than a preference is fixed in
Gea itself.
