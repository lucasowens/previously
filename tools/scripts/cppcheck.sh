#!/usr/bin/env bash
# Previously — run cppcheck over every project translation unit (all findings are errors).
#
#   tools/scripts/cppcheck.sh [build-dir]    default build dir: build/dev
#
# tests/ is excluded: cppcheck cannot parse doctest's macros. clang-tidy covers the tests.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

build_dir="${1:-${repo_root}/build/dev}"

cppcheck \
    --project="${build_dir}/compile_commands.json" \
    --file-filter="${repo_root}/engine/*" \
    --file-filter="${repo_root}/tools/*" \
    --enable=warning,style,performance,portability \
    --std=c++20 \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    --suppress=unmatchedSuppression \
    --suppress="*:${repo_root}/vendor/*" \
    --error-exitcode=1 \
    --quiet

echo "cppcheck: clean"
