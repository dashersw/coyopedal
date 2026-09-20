#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# The allocator wraps ESP-IDF's TLSF, so the test compiles against the IDF the
# firmware build actually used: the one recorded in the last build's
# project_description.json. An ambient IDF_PATH is the weakest source -- a
# profile that exports a version no longer installed should not decide what
# this test builds against -- so it is only consulted when there is no build to
# read. To point the test somewhere else for one run, pass --idf-path; a shell
# cannot tell an inline `IDF_PATH=... ` prefix from an exported one, which is
# why the override is an argument rather than the variable.
idf_path=""
optional=0
while (($#)); do
  case "$1" in
    --if-available)
      optional=1
      shift
      ;;
    --idf-path)
      idf_path="${2:?--idf-path needs a path}"
      shift 2
      ;;
    --idf-path=*)
      idf_path="${1#*=}"
      shift
      ;;
    *)
      echo "test-bank-allocator: unknown argument $1" >&2
      exit 2
      ;;
  esac
done
source=argument
if [[ -z "$idf_path" ]]; then
  # A glob, not `ls | head`: with pipefail an `ls` that matches nothing returns
  # 2, and set -e then killed this script before it could say why. A checkout
  # that has never built the firmware is the common case, and it reported
  # nothing at all.
  description=""
  for candidate in .gea/build/*/app-builds/nam-pedalboard/project_description.json; do
    if [[ -f "$candidate" ]]; then
      description="$candidate"
      break
    fi
  done
  if [[ -n "$description" ]]; then
    idf_path="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["idf_path"])' "$description")"
    source="$description"
  elif [[ -n "${IDF_PATH:-}" ]]; then
    idf_path="$IDF_PATH"
    source=IDF_PATH
  elif ((optional)); then
    echo "test-bank-allocator: SKIPPED, no ESP-IDF to build the allocator against"
    exit 0
  else
    echo "test-bank-allocator: pass --idf-path, set IDF_PATH, or run npm run build:firmware first" >&2
    exit 1
  fi
fi
tlsf="$idf_path/components/heap/tlsf"
if [[ ! -f "$tlsf/tlsf.c" ]]; then
  if ((optional)); then
    echo "test-bank-allocator: SKIPPED, no ESP-IDF at $idf_path (from $source)"
    exit 0
  fi
  echo "test-bank-allocator: no ESP-IDF at $idf_path (from $source)" >&2
  exit 1
fi
cc -std=c11 -g -fsanitize=address,undefined -I"$tlsf/include" -Isrc/audio/nam \
  "$tlsf/tlsf.c" src/native/nam_banks/bank_allocator.c src/audio/nam/runtime_allocation_plan.c tests/bank_allocator_test.c \
  -o build/bank_allocator_test
build/bank_allocator_test
