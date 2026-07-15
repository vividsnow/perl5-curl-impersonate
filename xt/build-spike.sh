#!/bin/sh
# Build curl-impersonate (lexiforest) to a user prefix. No sudo.
set -eu
VER="${CI_VERSION:-v1.5.6}"
PREFIX="${CI_PREFIX:-$PWD/.tmp/ci-prefix}"
SRC="$PWD/.tmp/curl-impersonate"
mkdir -p "$PWD/.tmp"
[ -d "$SRC" ] || git clone --depth 1 --branch "$VER" https://github.com/lexiforest/curl-impersonate.git "$SRC"
cd "$SRC"
rm -rf build && mkdir build && cd build
../configure --prefix="$PREFIX"
make build
make install
echo "PREFIX=$PREFIX"
ls -1 "$PREFIX/lib"/libcurl-impersonate* "$PREFIX/include/curl/curl.h" 2>&1
