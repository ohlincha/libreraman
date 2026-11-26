#!/usr/bin/env bash
# CI helper: prints the C++ version (project() in cpp/CMakeLists.txt) and, on a tag v*, warns if they differ.
set -euo pipefail
v=$(sed -n 's/^project(LibreRaman VERSION \([0-9][0-9.]*\).*/\1/p' "$(dirname "$0")/../CMakeLists.txt")
[ -n "$v" ] || { echo "::error::version not found in cpp/CMakeLists.txt" >&2; exit 1; }
if [[ "${GITHUB_REF:-}" == refs/tags/v* && "${GITHUB_REF#refs/tags/v}" != "$v" ]]; then
    echo "::warning::tag ${GITHUB_REF#refs/tags/} differs from the C++ version $v (cpp/CMakeLists.txt)" >&2
fi
echo "$v"
