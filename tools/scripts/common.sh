#!/usr/bin/env bash
# Previously — shared helpers for the developer scripts.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# The LLVM major version every machine and CI must use, so formatting never drifts.
required_llvm_major=23

# Finds an LLVM tool of the required major version: $LLVM_BIN, Homebrew, versioned name, PATH.
find_llvm_tool() {
    local name="$1"
    local candidate

    for candidate in \
        "${LLVM_BIN:-}/${name}" \
        "/opt/homebrew/opt/llvm/bin/${name}" \
        "/usr/local/opt/llvm/bin/${name}" \
        "$(command -v "${name}-${required_llvm_major}" || true)" \
        "$(command -v "${name}" || true)"; do
        if [[ -x "${candidate}" ]] && "${candidate}" --version | grep -q "version ${required_llvm_major}\."; then
            echo "${candidate}"
            return 0
        fi
    done

    echo "error: ${name} ${required_llvm_major}.x not found (set LLVM_BIN)" >&2
    return 1
}

# Lists every C++ source we own (vendor/ is never touched).
list_sources() {
    git -C "${repo_root}" ls-files --cached --others --exclude-standard -- \
        '*.cpp' '*.hpp' ':!:vendor/**'
}
