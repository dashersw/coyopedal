#!/usr/bin/env bash
# Prepare everything tools/null_test.py needs, under build/null-test*:
#   - NeuralAmpModelerCore and its `render` tool, the float reference renderer
#   - a Python venv with numpy, scipy and soundfile
#   - the original VoLum .nam files for the captures under test
#   - the DI to play into the device, copied to build/null-test/di.wav
#
#   bash scripts/null-test-setup.sh <guitar-di.wav> [capture ...]
#
# Captures are VoLum paths under rigs/, e.g. "Ampete One/V30-Ampt-4"; the
# default set is the factory Ampete and Herbert captures plus a Marshall 2204.
# The DI must be mono 48 kHz. See docs/NULL_TEST.md.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <guitar-di.wav> [\"<amp>/<capture>\" ...]" >&2
  exit 2
fi
di=$1
shift
captures=("$@")
if [[ ${#captures[@]} -eq 0 ]]; then
  captures=("Ampete One/V30-Ampt-4" "Diezel Herbert Mk1/V30-Herb-1" "Marshall 2204 1982/V30-2204-1")
fi
volum_tag=v1.2.3

mkdir -p build/null-test/models build/null-test/renders build/null-test/out
cp "$di" build/null-test/di.wav

core=build/NeuralAmpModelerCore
if [[ ! -d $core ]]; then
  git clone -q --depth 1 https://github.com/sdatkinson/NeuralAmpModelerCore.git "$core"
fi
git -C "$core" submodule update --init --depth 1 > /dev/null
if [[ ! -x $core/build-render/tools/render ]]; then
  cmake -S "$core" -B "$core/build-render" -DCMAKE_BUILD_TYPE=Release > /dev/null
  cmake --build "$core/build-render" --target render -j8 > /dev/null
fi

if [[ ! -x build/null-test-venv/bin/python ]]; then
  python3 -m venv build/null-test-venv
fi
build/null-test-venv/bin/pip install -q numpy scipy soundfile sounddevice

for capture in "${captures[@]}"; do
  file="build/null-test/models/$(basename "$capture").nam"
  if [[ ! -s $file ]]; then
    encoded=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "rigs/$capture.nam")
    curl -sfL -o "$file" "https://raw.githubusercontent.com/guitarlum/VoLum/$volum_tag/$encoded"
  fi
  echo "$file"
done
echo "render: $core/build-render/tools/render"
echo "python: build/null-test-venv/bin/python"
