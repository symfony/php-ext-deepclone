#!/usr/bin/env bash
set -euo pipefail

fuzz_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
fuzzer="$fuzz_dir/deepclone_from_array_fuzzer"

if [[ ! -x "$fuzzer" ]]; then
    echo "Build fuzz/deepclone_from_array_fuzzer first" >&2
    exit 1
fi

export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1:print_stacktrace=1"
exec "$fuzzer" "$@"
