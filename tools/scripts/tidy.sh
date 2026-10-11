#!/usr/bin/env bash
# Previously — run clang-tidy over every project source (all findings are errors).
#
#   tools/scripts/tidy.sh [build-dir]    default build dir: build/dev (needs compile_commands.json)

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

build_dir="${1:-${repo_root}/build/dev}"
clang_tidy="$(find_llvm_tool clang-tidy)"
run_clang_tidy="$(dirname "${clang_tidy}")/run-clang-tidy"

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
    echo "error: ${build_dir}/compile_commands.json missing; configure first (cmake --preset dev)" >&2
    exit 1
fi

# Translation units only (headers are checked through them via HeaderFilterRegex).
sources=()
while IFS= read -r file; do
    [[ "${file}" == *.cpp ]] && sources+=("${repo_root}/${file}")
done < <(list_sources | grep -v '^rack/\|^firmware/')

extra_args=()
if [[ "$(uname)" == "Darwin" ]]; then
    # Homebrew clang-tidy needs the macOS SDK to find the system headers Apple clang uses.
    extra_args+=("-extra-arg=-isysroot$(xcrun --show-sdk-path)")
fi

"${run_clang_tidy}" -quiet -p "${build_dir}" -clang-tidy-binary "${clang_tidy}" \
    "${extra_args[@]}" "${sources[@]}"

echo "tidy: ${#sources[@]} translation units clean"
