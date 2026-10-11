#!/usr/bin/env bash
# Previously — format every project source with clang-format.
#
#   tools/scripts/format.sh          rewrite files in place
#   tools/scripts/format.sh --check  report differences and fail, change nothing (CI, pre-commit)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

clang_format="$(find_llvm_tool clang-format)"
sources=()
while IFS= read -r file; do
    sources+=("${file}")
done < <(list_sources)

cd "${repo_root}"

if [[ "${1:-}" == "--check" ]]; then
    "${clang_format}" --dry-run --Werror "${sources[@]}"
    echo "format: ${#sources[@]} files clean"
else
    "${clang_format}" -i "${sources[@]}"
    echo "format: ${#sources[@]} files formatted"
fi
