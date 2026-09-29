#!/usr/bin/env bash
set -euo pipefail

fuzz_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$fuzz_dir/.." && pwd)"

if [[ -z "${PHP_SRC:-}" ]]; then
    echo "PHP_SRC is required and must point to a configured php-src tree" >&2
    exit 1
fi
php_src="$PHP_SRC"

if [[ ! -f "$php_src/sapi/fuzzer/fuzzer-sapi.c" ]]; then
    echo "PHP_SRC must point to a configured php-src tree with --enable-fuzzer" >&2
    exit 1
fi
if [[ ! -f "$project_dir/modules/deepclone.so" ]]; then
    echo "Build modules/deepclone.so first" >&2
    exit 1
fi

exec make -C "$php_src" -f Makefile -f "$fuzz_dir/php-fuzzer.mk" \
    DEEPCLONE_PROJECT="$project_dir" deepclone-fuzzer
