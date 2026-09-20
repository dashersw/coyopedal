---
name: editing-the-gea-ui
description: Use when changing the touchscreen UI — screens, JSX components, styles, PedalboardStore, the board API boundary, or the browser preview — or when a UI change needs to be seen in a browser.
---

# The Gea UI

A single TypeScript/JSX app runs on both targets. The web build uses Vite and `@geajs/core`. On
the ESP32, `@geastack/compiler` lowers it to native C++, and there is no JS VM.

## Map

| Path                                   | Role                                                                        |
| -------------------------------------- | --------------------------------------------------------------------------- |
| `src/ui/index.tsx`                     | `mount(App)` entry                                                          |
| `src/ui/components/*.tsx` + `.css`     | One stylesheet per component; `App.css` holds only shared shell/utilities   |
| `src/ui/stores/PedalboardStore.ts`     | Navigation, preset actions, view of board state (`PageStore.ts` for paging) |
| `src/ui/board.ts`                      | Typed boundary to audio controls and preset storage                         |
| `src/preview/`                         | Browser-only fixture for `board.ts`, plus mouse-drag scrolling              |
| `scripts/panel-host-plugin.mjs`        | Declares the board boundary to the compiler (`gea.compilerPlugins`)         |
| `src/native/ui/board_bridge.cpp`       | Native implementation of the board API                                      |
| `src/native/ui/controls.c`, `task.cpp` | Audio control state; tuner and preset-save pumps around the frame loop      |

## Adding a board call

Add it in three places: the type in `src/ui/board.ts`, the fixture in `src/preview/`, and the
native side in `board_bridge.cpp`. Also declare it in `scripts/panel-host-plugin.mjs` if the
plugin enumerates it. Keep the store the only caller.

## Seeing it

- `npm run dev` starts the Vite server. `.claude/launch.json` defines `dev` on port 3000, so use
  the preview tools with that name. The layout is 251 × 205 logical px, scaled ×2 to the
  502 × 410 panel, with a 130 px physical corner radius.
- `npm run preview:panel` writes `build/ui-preview/`.
- `node --test tests/gea-store.test.mjs` and `npm run check:frontend`.
- `npm run build:firmware` is the only check of the native lowering. Compilation must keep
  dynamic fallback disabled.

## Don'ts

- Don't edit generated C++ or rasterised assets under `.gea/build/`.
- Don't add a JS runtime, a second compiler invocation, a local panel/display component or the
  old hand-written screen renderer.
- Don't enable Gea's own network services. Wi-Fi and BLE belong to the maintenance service.
- The browser preview doesn't show device frame rate, PSRAM headroom or audio impact.
  UI boot has caused audio misses before (see `measuring-pedal-audio`).
- Fonts: `assets/fonts/Saira-Bold-Pedal.otf`, Saira Bold with icon glyphs drawn in by
  `node tools/font_glyphs.mjs` (edit its `GLYPHS` table and rerun). The panel only has printable ASCII and a short list of extra
  codepoints (`embeddedFontExtraCodepoints` in `@geastack/core`); icons are glyphs drawn there
  (‹ › are chevrons). Don't use SVG, images or rotated borders for icons. Public asset paths must be literals
  so both Vite and the embedded asset linker resolve them.
