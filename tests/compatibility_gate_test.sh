#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

set -Eeuo pipefail

if (( $# != 3 )); then
  printf 'usage: %s BLOX7 UNKNOWN_ELF COMPATIBILITY_MANIFEST\n' "$0" >&2
  exit 2
fi

blox7_binary="$1"
unknown_elf="$2"
compatibility_manifest="$3"

temporary_root="$(mktemp -d)"
trap 'rm -rf "${temporary_root}"' EXIT

set +e
mkdir -p \
  "${temporary_root}/run" \
  "${temporary_root}/assets" \
  "${temporary_root}/config" \
  "${temporary_root}/data" \
  "${temporary_root}/cache" \
  "${temporary_root}/state" \
  "${temporary_root}/auth"
output="$(
  env -i \
    PATH="${PATH}" \
    HOME="${HOME}" \
    XDG_RUNTIME_DIR="${temporary_root}/run" \
    BLOX7_ISOLATED_CANARY=1 \
    BLOX7_ALLOW_NO_COOKIE_LUA_APP=1 \
    BLOX7_CONFIG_ROOT="${temporary_root}/config" \
    BLOX7_DATA_ROOT="${temporary_root}/data" \
    BLOX7_CACHE_ROOT="${temporary_root}/cache" \
    BLOX7_STATE_ROOT="${temporary_root}/state" \
    BLOX7_AUTH_ROOT="${temporary_root}/auth" \
    BLOX7_COOKIE_FILE="${temporary_root}" \
    BLOX7_ASSET_PATH="${temporary_root}/assets" \
    BLOX7_SKIP_UPDATE_CHECK=1 \
    BLOX7_AUTO_EXIT_AFTER_PRESENT_MS=5000 \
    ROBLOX_LIB_PATH="${unknown_elf}" \
    BLOX7_COMPATIBILITY_MANIFEST="${compatibility_manifest}" \
    "${blox7_binary}" 2>&1
)"
status=$?
set -e

if (( status == 0 )); then
  printf 'unknown payload unexpectedly succeeded\n%s\n' "${output}" >&2
  exit 1
fi
if ! grep -Fq 'Unsupported Roblox Build ID' <<<"${output}"; then
  printf 'missing fail-closed Build-ID diagnostic\n%s\n' "${output}" >&2
  exit 1
fi
if grep -Fq 'unknown Roblox fixture reached native loading' <<<"${output}"; then
  printf 'unknown payload reached native loading\n%s\n' "${output}" >&2
  exit 1
fi

printf '%s\n' "${output}"
