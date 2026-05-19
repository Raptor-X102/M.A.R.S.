#!/usr/bin/env bash

set -euo pipefail

clang_format_bin="${CLANG_FORMAT_BIN:-}"

if [ -z "${clang_format_bin}" ]; then
    if command -v clang-format-22 >/dev/null 2>&1; then
        clang_format_bin="clang-format-22"
    elif command -v clang-format >/dev/null 2>&1 && clang-format --version | grep -Eq 'version 22([. ]|$)'; then
        clang_format_bin="clang-format"
    else
        echo "error: clang-format 22 was not found in PATH" >&2
        echo "hint: install clang-format 22 or set CLANG_FORMAT_BIN to the right binary" >&2
        exit 127
    fi
elif ! command -v "${clang_format_bin}" >/dev/null 2>&1; then
    echo "error: '${clang_format_bin}' was not found in PATH" >&2
    exit 127
fi

mapfile -d '' files < <(
    find include src -type f \
        \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.h' -o -name '*.hh' \) \
        -print0 | sort -z
)

if [ "${#files[@]}" -eq 0 ]; then
    echo "No C/C++ source files found to format."
    exit 0
fi

"${clang_format_bin}" --version
"${clang_format_bin}" -i "${files[@]}"

if [ "${1:-}" = "--check" ]; then
    git diff --exit-code -- "${files[@]}"
fi
