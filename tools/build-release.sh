#!/bin/sh
# Run in a fresh Alpine 3.23 container with the repository and an output directory mounted.
set -eu

tag=$1
output=$2
source_dir=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
jobs=$(getconf _NPROCESSORS_ONLN)

apk add --no-cache build-base pkgconf meson curl xz gperf \
    libx11-dev libxinerama-dev libxext-dev cairo-dev fontconfig-dev glib-dev \
    harfbuzz-dev freetype-dev fribidi-dev \
    libx11-static libxext-static cairo-static glib-static \
    harfbuzz-static freetype-static fribidi-static pixman-static libxcb-static \
    bzip2-static expat-static libpng-static brotli-static zlib-static pcre2-static \
    util-linux-static gettext-static graphite2-static

fetch_source() {
    url=$1
    file=${url##*/}
    curl --fail --silent --show-error --location --retry 3 -o "$file" "$url"
    printf '%s  %s\n' "$2" "$file" | sha256sum -c -
    tar -xf "$file"
}

mkdir "$work/deps" "$work/launch"
cd "$work/deps"
fetch_source https://www.x.org/releases/individual/lib/libXau-1.0.12.tar.xz \
    74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb
fetch_source https://download.gnome.org/sources/pango/1.56/pango-1.56.4.tar.xz \
    17065e2fcc5f5a5bdbffc884c956bfc7c451a96e8c4fb2f8ad837c6413cb5a01
fetch_source https://distfiles.alpinelinux.org/distfiles/v3.23/libeconf-0.8.3.tar.gz \
    c8855f08d64c06b57a5028a14eec6ddde5e35124f9718031d592b44d8e091f83
fetch_source https://distfiles.alpinelinux.org/distfiles/edge/fontconfig-2.18.4.tar.xz \
    95013637eff5f81d4871dd16cd098c273264c16d0f9f0b27cc43b81b2794bee3

(
    cd libXau-1.0.12
    ./configure --prefix=/usr/local --enable-static --disable-shared
    make -j "$jobs" install
)
meson_static() {
    src=$1
    shift
    meson setup "$src/_build" "$src" --prefix=/usr/local --libdir=lib \
        --default-library=static --buildtype=release --wrap-mode=nofallback "$@"
    meson compile -C "$src/_build" -j "$jobs"
    meson install -C "$src/_build" --no-rebuild
}
meson_static pango-1.56.4 -Dcairo=enabled -Dintrospection=disabled \
    -Ddocumentation=false -Dman-pages=false -Dxft=disabled -Dlibthai=disabled -Dsysprof=disabled
meson_static libeconf-0.8.3
# Pango 1.56 builds with Alpine's 2.17 headers. Link to Fontconfig 2.18 for
# compatibility with newer font rules on rolling distributions.
(
    cd fontconfig-2.18.4
    ./configure --prefix=/usr/local --sysconfdir=/etc --localstatedir=/var \
        --enable-static --disable-shared --disable-docs --disable-nls
    make -j "$jobs" install
)

cd "$source_dir"
cp -R Makefile LICENSE src assets vendor config.ini "$work/launch"
cd "$work/launch"
make -j "$jobs" STATIC=1 VERSION="${tag#v}" build/launch

mkdir -p "$output"
asset=launch-linux-$(uname -m)
strip -o "$output/$asset" build/launch
readelf -lW "$output/$asset" > "$work/program-headers"
readelf -dW "$output/$asset" > "$work/dynamic-section"
if grep -Eq '[[:space:]]INTERP[[:space:]]' "$work/program-headers" ||
        grep -Fq '(NEEDED)' "$work/dynamic-section"; then
    printf '%s\n' 'Release binary has dynamic dependencies.' >&2
    exit 1
fi
test "$("$output/$asset" --version)" = "launch ${tag#v}"

# Redistribute the library sources and the inputs needed to relink Launch.
relink=$work/$asset-relink
python3 "$source_dir/tools/release-sources.py" build/launch.map "$relink"
mkdir "$relink/objects" "$relink/notices"
cp build/*.o "$relink/objects"
cp "$work/deps/"*.tar.* "$relink/sources"
cp LICENSE assets/*-LICENSE vendor/cui/CLAY-LICENSE "$relink/notices"
cp vendor/cui/LICENSE "$relink/notices/CUI-LICENSE"
cp "$source_dir/tools/relink.txt" "$relink/README.txt"
cc -dumpfullversion > "$relink/gcc-version"
tar -czf "$output/$asset-relink.tar.gz" -C "$work" "$asset-relink"
(cd "$output" && sha256sum "$asset" > "$asset.sha256")
