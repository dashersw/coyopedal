#!/usr/bin/env bash
set -euo pipefail

# Builds web/ — the panel as a WASM program.
#
# This is the SIMULATOR target, not `gea build --target web`. The difference
# matters: the DOM target recompiles the TSX onto @geajs/core and lets the
# browser lay out and paint, so it drifts from the board exactly where you would
# want to trust it. Here geatsc lowers the same TSX to C++ and emcc links it
# with the same Gea layout/paint engine the firmware runs; the browser only
# supplies a canvas and the pointer events. What the page shows is what the
# panel shows.
#
# Three things this has to state that `gea simulate` does not. The framework
# packages come from THIS repo's node_modules, because the simulator package
# carries its own older installation and an app compiled against both sees two
# copies of every header. The app is web/, which the CLI would not find on its
# own: app discovery looks at the repo root's package.json and at apps/, and
# GEA_EXTRA_APP_DIRS is the documented way to name another one. And geatsc has
# to be pointed at the gea host shims — see GEATSC2_GEA_PLUGIN below.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PACKAGES="$REPO/node_modules/@geastack"
BUILD="$REPO/.gea/build/web"

# The script that drives this build ships in @geastack/simulator, so the
# installed package is what a clone uses and nothing outside the repo has to
# exist. A checkout beside it is the framework-development case -- editing the
# driver and this app in one loop -- and GEA_SIMULATOR_DIR names it.
SIMULATOR="${GEA_SIMULATOR_DIR:-}"
if [[ -z "$SIMULATOR" ]]; then
  SIMULATOR="$(node -e "process.stdout.write(require('path').dirname(require.resolve('@geastack/simulator/package.json')))" 2> /dev/null || true)"
fi
if [[ -z "$SIMULATOR" ]]; then
  SIMULATOR="$REPO/../../geastack/simulator"
fi

if [[ ! -f "$SIMULATOR/targets/web/build-web.sh" ]]; then
  echo "No @geastack/simulator at $SIMULATOR; npm install, or set GEA_SIMULATOR_DIR." >&2
  exit 1
fi
if ! command -v emcc > /dev/null 2>&1; then
  echo "emcc is not on PATH; source the Emscripten SDK's emsdk_env.sh." >&2
  exit 1
fi

# geatsc reads the host type table — which TypeScript name is carried by which
# C++ type — from the gea plugin package, and resolves it with a require rooted
# at the compiler's own install. A compiler checked out beside the repo rather
# than installed into it therefore finds nothing, and a missing package is a
# configuration rather than a defect, so the table comes back empty and the
# build fails much later, at certification, as "the target manifest registers no
# native-boundary Document@1" — the TypeScript names, unclaimed, exactly as
# plugins/gea/host.ts predicts in its own comment. GEATSC2_GEA_PLUGIN is the
# documented override. It must name the shim module WITHOUT its extension: the
# canvas interop is loaded by swapping the "host-shims" suffix for "cpp-ir".
#
# GEA_WEB_DEVICE_PIXEL_RATIO is the ratio the font generator bakes atlases for,
# and it has to be the ratio the page actually renders at: web/index.html runs
# the engine at 2 x OVERSAMPLE = 4, twice the board's own 2, so the layout is
# supersampled. Baking for 2 left every size an inexact request at run time,
# answered by the nearest atlas — which for the 22px preset name is the tuner's
# 52px one, narrowed to the twenty characters a note name needs, so the name
# drew as `?`. Keep this equal to web/index.html DEVICE_PIXEL_RATIO.
GEATSC2_GEA_PLUGIN="$PACKAGES/geatsc-plugin-gea/dist/host-shims" \
  GEA_APPS_ROOT="$REPO" \
  GEA_EXTRA_APP_DIRS="$REPO/web" \
  GEA_CORE="$PACKAGES/core" \
  GEA_CORE_DIR="$PACKAGES/core" \
  GEA_COMPILER_DIR="$PACKAGES/compiler" \
  GEA_HOST_DIR="$PACKAGES/host" \
  GEA_ENGINE_DIR="$PACKAGES/engine" \
  GEA_ELEMENTS_DIR="$PACKAGES/elements" \
  GEA_GEAOS_PACKAGE_DIR="$PACKAGES/geaos" \
  GEA_WEB_GENERATED_ROOT="$BUILD/generated" \
  GEA_WEB_DIST_ROOT="$BUILD/dist" \
  GEA_WEB_PUBLIC_ROOT="$BUILD/public" \
  GEA_WEB_DEVICE_PIXEL_RATIO=4 \
  bash "$SIMULATOR/targets/web/build-web.sh" nam-pedalboard-web

# The two factory images the page installs into the board, packed exactly as the
# esp32 prebuild packs them (gea.targets.esp32.prebuild). The page reads the same
# bytes the board is flashed with, so they have to be current with the sources.
node "$REPO/scripts/pack-factory-assets.mjs"

# web/ is the deployable site root: everything the page loads sits under it, so
# the same directory serves locally and uploads to a static host unchanged.
mkdir -p "$REPO/web/dist/factory"
cp "$BUILD/dist/nam-pedalboard-web/module.js" "$REPO/web/dist/module.js"
cp "$BUILD/dist/nam-pedalboard-web/module.wasm" "$REPO/web/dist/module.wasm"
cp "$REPO/build/factory/models.bin" "$REPO/web/dist/factory/models.bin"
cp "$REPO/build/factory/presets.json" "$REPO/web/dist/factory/presets.json"
# Every file under web/dist gets a version on its URL, because the page names
# them and the page is the only thing that is never cached. module.js and
# module.wasm share one version: they are one program in two files -- the wasm
# imports the EM_JS functions the glue defines -- so a browser holding the
# previous module.js from an earlier visit and pairing it with this .wasm does
# not degrade, the page does not start at all. See the note next to ASSETS in
# web/index.html.
node "$REPO/scripts/stamp-web-build.mjs" "$REPO/web/index.html" "$REPO/web/dist"

echo "Serve it and open /web/:"
echo "  npm run serve:web"
