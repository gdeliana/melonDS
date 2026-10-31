#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-build-appimage}
output_dir=${2:-$build_dir/dist}
jobs=${3:-4}

fail() { echo "AppImage: $*" >&2; exit 1; }
[[ $(uname -s) == Linux ]] || fail 'packaging requires Linux.'
case $(uname -m) in
    x86_64|aarch64) arch=$(uname -m) ;;
    *) fail "unsupported architecture: $(uname -m)" ;;
esac
[[ $jobs =~ ^[1-9][0-9]*$ ]] || fail 'JOBS must be a positive integer.'
for tool in cmake ninja c++ curl file desktop-file-validate; do
    command -v "$tool" >/dev/null || fail "missing $tool; install the build dependencies listed in BUILD.md."
done

if [[ -z ${QMAKE:-} ]]; then
    QMAKE=$(command -v qmake6 || command -v qmake || true)
fi
[[ -n $QMAKE && -x $QMAKE ]] || fail 'Qt 6 qmake is required; set QMAKE to its full path.'
[[ $("$QMAKE" -query QT_VERSION) == 6.* ]] || fail 'this AppImage target requires Qt 6.'
export QMAKE

[[ $build_dir == /* ]] || build_dir=$repo_root/$build_dir
[[ $output_dir == /* ]] || output_dir=$repo_root/$output_dir
mkdir -p -- "$build_dir" "$output_dir"
build_dir=$(cd -- "$build_dir" && pwd)
output_dir=$(cd -- "$output_dir" && pwd)
tools_dir=$build_dir/appimage-tools
mkdir -p -- "$tools_dir"

# Cache official tools, replacing the destination only after a complete download.
fetch_tool() {
    local project=$1 name=$2 destination=$tools_dir/$2
    if [[ ! -x $destination ]]; then
        curl --fail --location --retry 3 \
            "https://github.com/linuxdeploy/$project/releases/download/continuous/$name" \
            --output "$destination.download"
        chmod +x -- "$destination.download"
        mv -- "$destination.download" "$destination"
    fi
}
fetch_tool linuxdeploy "linuxdeploy-$arch.AppImage"
fetch_tool linuxdeploy-plugin-qt "linuxdeploy-plugin-qt-$arch.AppImage"

git_hash=$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo source-archive)
git_branch=$(git -C "$repo_root" symbolic-ref --quiet --short HEAD 2>/dev/null || echo detached)
cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DUSE_QT6=ON -DMELONDS_EMBED_BUILD_INFO=ON \
    "-DMELONDS_GIT_HASH=$git_hash" "-DMELONDS_GIT_BRANCH=$git_branch" \
    '-DMELONDS_BUILD_PROVIDER=Local AppImage'
cmake --build "$build_dir" --parallel "$jobs"

# A fresh staging directory prevents stale plugins from earlier builds leaking
# into the package. The trap removes only this script's mktemp-created directory.
stage_root=$(mktemp -d "$build_dir/appimage-stage.XXXXXX")
trap 'rm -rf -- "$stage_root"' EXIT
appdir=$stage_root/AppDir
DESTDIR="$appdir" cmake --install "$build_dir"

# Include the SVG icon engine and available headless/Wayland platform plugins.
# Explicit user overrides are honored for custom Qt installations.
export EXTRA_QT_MODULES=${EXTRA_QT_MODULES:-svg}
if [[ -z ${EXTRA_PLATFORM_PLUGINS+x} ]]; then
    plugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)
    extra_platforms=()
    for plugin in libqoffscreen.so libqwayland-egl.so libqwayland-generic.so; do
        [[ ! -f $plugins/platforms/$plugin ]] || extra_platforms+=("$plugin")
    done
    EXTRA_PLATFORM_PLUGINS=$(IFS=';'; echo "${extra_platforms[*]}")
fi
export EXTRA_PLATFORM_PLUGINS
export APPIMAGE_EXTRACT_AND_RUN=1
export LDAI_OUTPUT=$output_dir/melonDS-$arch.AppImage
export ARCH=$arch

cd -- "$output_dir"
"$tools_dir/linuxdeploy-$arch.AppImage" --appdir "$appdir" --plugin qt --output appimage
[[ -s $LDAI_OUTPUT && -x $LDAI_OUTPUT ]] || fail 'packager did not produce an executable AppImage.'
install -m 755 -- "$repo_root/run-melonds.sh" "$output_dir/run-melonds.sh"
echo "AppImage ready: $LDAI_OUTPUT"
echo "Portable launcher: $output_dir/run-melonds.sh"
