#!/bin/sh
set -eu

die() {
    printf 'launch installer: %s\n' "$*" >&2
    exit 1
}

download() {
    curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' "$@"
}

main() {
    [ "$#" -le 1 ] || die 'Usage: sh install.sh [vX.Y.Z]'
    case "$(uname -s)/$(uname -m)" in
        Linux/x86_64) arch=x86_64 ;;
        Linux/aarch64|Linux/arm64) arch=aarch64 ;;
        *) die 'Supported platforms are Linux x86_64 and AArch64.' ;;
    esac

    releases=https://github.com/mattmezza/launch/releases
    tag=${1:-}
    if [ -z "$tag" ]; then
        url=$(download --output /dev/null --write-out '%{url_effective}' "$releases/latest") ||
            die 'Could not find the latest release.'
        tag=${url#"$releases/tag/"}
    fi
    if [ -z "$tag" ] || [ "$(printf '%s\n' "$tag" | LC_ALL=C grep -Ex 'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)')" != "$tag" ]; then
        die 'Expected a release tag such as v0.1.0.'
    fi

    bin=${LAUNCH_INSTALL_DIR:-"$HOME/.local/bin"}
    case "$bin" in
        /*) ;;
        *) die 'LAUNCH_INSTALL_DIR must be an absolute path.' ;;
    esac
    mkdir -p "$bin"
    tmp=$(mktemp -d "$bin/.launch.XXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 1' HUP INT TERM

    asset=launch-linux-$arch
    base=$releases/download/$tag
    download --output "$tmp/$asset" "$base/$asset"
    download --output "$tmp/$asset.sha256" "$base/$asset.sha256"
    checksum=$(cd "$tmp" && sha256sum "$asset")
    [ "$checksum" = "$(cat "$tmp/$asset.sha256")" ] || die 'Checksum verification failed.'
    chmod 755 "$tmp/$asset"
    [ "$("$tmp/$asset" --version)" = "launch ${tag#v}" ] ||
        die "The downloaded executable failed the $tag version check."
    [ ! -d "$bin/launch" ] && [ ! -d "$bin/launch-dmenu" ] ||
        die "An installation path in $bin is a directory."

    ln -s launch "$tmp/launch-dmenu"
    mv -fT "$tmp/launch-dmenu" "$bin/launch-dmenu"
    mv -fT "$tmp/$asset" "$bin/launch"
    printf 'Installed Launch %s in %s\n' "$tag" "$bin"
    case ":${PATH:-}:" in
        *":$bin:"*) ;;
        *) printf 'Add %s to PATH to run launch and launch-dmenu.\n' "$bin" ;;
    esac
}

main "$@"
