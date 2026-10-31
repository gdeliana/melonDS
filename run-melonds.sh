#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
binary=${MELONDS_BINARY:-}
import_settings=1

usage() {
    cat <<'EOF'
Usage: ./run-melonds.sh [--binary PATH] [--fresh] [--] [melonDS arguments...]

Creates portable settings beside melonDS and starts it. Existing portable
settings are preserved. On first use, imports the system melonDS.toml if found.

  --binary PATH  Choose an AppImage or native melonDS executable explicitly.
  --fresh        Skip importing system settings; existing portable settings stay.
  --help         Show launcher help. Use -- --help for melonDS help.

Place this script beside your AppImage/executable, or run it from this checkout.
MELONDS_BINARY can also select the executable. All remaining arguments are
forwarded unchanged; relative ROM paths use your current working directory.
EOF
}
fail() { echo "Portable melonDS: $*" >&2; exit 1; }
while (($#)); do
    case $1 in
        --binary)
            (($# >= 2)) || fail '--binary requires a path.'
            [[ -n $2 ]] || fail '--binary requires a nonempty path.'
            binary=$2; shift 2 ;;
        --fresh) import_settings=0; shift ;;
        --help|-h) usage; exit 0 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

if [[ -z $binary ]]; then
    # Prefer the package beside the launcher. Keep normal development builds
    # usable too, without depending on the caller's current directory.
    for candidate in \
        "$script_dir/melonDS-$(uname -m).AppImage" \
        "$script_dir/melonDS.AppImage" \
        "$script_dir/melonDS" \
        "$script_dir/build-appimage/dist/melonDS-$(uname -m).AppImage" \
        "$script_dir/build/melonDS"; do
        if [[ -f $candidate ]]; then binary=$candidate; break; fi
    done
fi
[[ -n $binary && -f $binary ]] || fail 'executable not found. Use --binary PATH, or build with make appimage CONTAINER=melonds-gpu.'
binary=$(realpath -- "$binary")
if [[ ! -x $binary ]]; then
    chmod u+x -- "$binary" || fail "cannot make executable: $binary"
fi

source_config=${XDG_CONFIG_HOME:-$HOME/.config}/melonDS/melonDS.toml
case $binary in
    *.AppImage)
        config_root=$binary.config
        settings_dir=$config_root/melonDS
        # Explicitly set this as well as creating the companion directory, so
        # extraction-based AppImage launches use the same portable settings.
        export XDG_CONFIG_HOME=$config_root
        ;;
    *) settings_dir=$(dirname -- "$binary")/portable ;;
esac
mkdir -p -- "$settings_dir" || fail "cannot create settings directory: $settings_dir"
[[ -w $settings_dir ]] || fail "settings directory is not writable: $settings_dir"

config_file=$settings_dir/melonDS.toml
if ((import_settings)) && [[ ! -e $config_file && ! -L $config_file && -f $source_config ]]; then
    # -n also protects a file another launcher may have created in the meantime.
    cp -n -- "$source_config" "$config_file"
fi

echo "Portable settings: $settings_dir" >&2
exec "$binary" "$@"
