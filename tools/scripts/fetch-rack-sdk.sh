#!/usr/bin/env bash
# Previously — download the VCV Rack SDK into .deps/Rack-SDK, pinned by version and checksum.
#
# Used by CI and by anyone without a Rack source tree. Locally you can instead build against a
# Rack checkout: make -C rack RACK_DIR=~/Source/Rack

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

rack_sdk_version="2.6.6"

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)  platform="lin-x64";   sha256="420da2452def7b195f98e3a0650c35f1e0f058a98a71840b4347d3d203618532" ;;
    Darwin-arm64)  platform="mac-arm64"; sha256="29414e52417992cbafa47e30f947c3c0c7a34e5c424bb83c5a0af8c24840481f" ;;
    Darwin-x86_64) platform="mac-x64";   sha256="9b8b0d7582ca25fac879f8f64de40f2481df2fd903b65f3abb5d6801689060ec" ;;
    *) echo "error: no pinned Rack SDK for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

deps_dir="${repo_root}/.deps"
archive="${deps_dir}/Rack-SDK-${rack_sdk_version}-${platform}.zip"
stamp="${deps_dir}/Rack-SDK/.version"

if [[ -f "${stamp}" && "$(cat "${stamp}")" == "${rack_sdk_version}-${platform}" ]]; then
    echo "rack sdk: ${rack_sdk_version} (${platform}) already present"
    exit 0
fi

mkdir -p "${deps_dir}"
curl -fsSL "https://vcvrack.com/downloads/Rack-SDK-${rack_sdk_version}-${platform}.zip" -o "${archive}"

if command -v sha256sum > /dev/null; then
    actual="$(sha256sum "${archive}" | cut -d' ' -f1)"
else
    actual="$(shasum -a 256 "${archive}" | cut -d' ' -f1)"
fi

if [[ "${actual}" != "${sha256}" ]]; then
    echo "error: Rack SDK checksum mismatch (expected ${sha256}, got ${actual})" >&2
    rm -f "${archive}"
    exit 1
fi

rm -rf "${deps_dir}/Rack-SDK"
unzip -q "${archive}" -d "${deps_dir}"
rm -f "${archive}"
echo "${rack_sdk_version}-${platform}" > "${stamp}"

echo "rack sdk: ${rack_sdk_version} (${platform}) installed in .deps/Rack-SDK"
