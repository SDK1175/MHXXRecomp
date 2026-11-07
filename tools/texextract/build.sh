#!/bin/sh
# Builds build/mhxx-texextract(.exe) (the same as `make texextract`). Run from the repository folder, in MSYS2 UCRT64
# on Windows: sh tools/texextract/build.sh
set -e
mkdir -p build
SRC="tools/texextract/texextract.c runtime/nvn/tegra.c runtime/nvn/formats.c runtime/nvn/bcn.c runtime/nvn/astc.c"
case "$(uname -s)" in
MINGW*|MSYS*) clang -O2 -o build/mhxx-texextract.exe $SRC -fuse-ld=lld -static-libgcc -Wl,-Bstatic -lz -Wl,-Bdynamic ;;
*) clang -O2 -o build/mhxx-texextract $SRC -lz ;;
esac
echo "built: build/mhxx-texextract"
