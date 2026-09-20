#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
for spec in 'uac2_test hdx-dma4-configuration' 'xtone_test xtone-pro-1.1.0-configuration'; do
  read -r test fixture <<< "$spec"
  c++ -std=c++20 -O1 -g -fsanitize=address,undefined -Isrc "tests/usb/$test.cpp" -o "build/$test"
  "build/$test" "tests/usb/$fixture.bin"
done
build/xtone_test tests/usb/xtone-pro-1.1.0-full-speed.bin full-speed
# Needs the ESP-IDF the firmware was built against, which a checkout that has
# only ever run host tests does not have. CI runs it unconditionally in the
# firmware job, where the IDF and a build both exist.
bash scripts/test-bank-allocator.sh --if-available
node --test tests/preset-preview.test.mjs
node --test tests/gea-store.test.mjs
node --test tests/amp-browser.test.mjs
npm run check:frontend
# The native lowering of this same TSX is checked by the firmware build, which
# the gea CLI owns; nothing here re-invokes the compiler with its own flags.
c++ -std=c++20 -O2 -Isrc src/audio/effects.cpp tests/effects_test.cpp -o build/effects_test
cjson=third_party/cjson
cc -DCJSON_NESTING_LIMIT=32 -c "$cjson/cJSON.c" -o build/cjson.o
# The preset document's reader and writer, as C, which is what the firmware
# compiles it as -- the panel preset test reads back what it writes.
cc -std=c11 -O2 -Isrc/native/storage -Isrc -I"$cjson" -c src/native/storage/preset_json.c -o build/preset_json.o
c++ -std=c++20 -O2 -Itests/preset_stubs -Isrc/native/storage -Isrc/native/ui -Isrc -I"$cjson" src/native/storage/panel_presets.cpp src/audio/effects.cpp tests/panel_presets_test.cpp build/preset_json.o build/cjson.o -o build/panel_presets_test
build/panel_presets_test
build/effects_test
c++ -std=c++20 -O2 -Isrc src/audio/effects.cpp tests/effects_lifecycle_test.cpp -o build/effects_lifecycle_test
build/effects_lifecycle_test
c++ -std=c++20 -O2 -Isrc/native/storage -Isrc/native/ui -I"$cjson" src/native/storage/nam_json.cpp tests/nam_parser_cli.cpp build/cjson.o -o build/nam_parser_test
python3 -m unittest discover -s tests -v

# The host cache check uses the S3 engine's scalar reference kernels.
link_gc=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then
  link_gc=-Wl,-dead_strip
fi
c++ -O3 -std=c++20 -ffp-contract=off -fcx-limited-range \
  -DCOYOPEDAL_PEDAL_S3_SPLIT_LAYER=8 -ffunction-sections -fdata-sections \
  "$link_gc" -Isrc/audio/nam -Isrc src/audio/nam/nam_a2_full_s3_native.cpp \
  tests/nam/prepared_model_test.cpp -o build/prepared_model_test
build/prepared_model_test assets/models/volum-ampete-4-v30.namb
