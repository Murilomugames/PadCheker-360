#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
XSDK_DIR="${XSDK_DIR:-$HOME/xsdk}"
XSDK_TOOLCHAIN="${XSDK_TOOLCHAIN:-$HOME/xsdk-gcc}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

fail() {
    printf 'ERRO: %s\n' "$1" >&2
    exit 1
}

[[ -f "$XSDK_DIR/cmake/xsdk.cmake" ]] || fail \
    "Nao encontrei $XSDK_DIR/cmake/xsdk.cmake. Clone o xsdk em ~/xsdk ou defina XSDK_DIR."
[[ -x "$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-gcc" ]] || fail \
    "Nao encontrei o compilador $XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-gcc. Construa a toolchain do xsdk primeiro."
[[ -f "$XSDK_DIR/build/libxsdk.a" ]] || fail \
    "Nao encontrei $XSDK_DIR/build/libxsdk.a. Entre na pasta do xsdk e execute make -j\$(nproc)."

command -v cmake >/dev/null 2>&1 || fail "CMake nao esta instalado."
command -v ninja >/dev/null 2>&1 || fail "Ninja nao esta instalado."

cmake --version | head -n 1
printf 'Projeto: %s\n' "$ROOT"
printf 'xsdk: %s\n' "$XSDK_DIR"
printf 'Toolchain: %s\n' "$XSDK_TOOLCHAIN"

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$XSDK_DIR/cmake/xsdk.cmake" \
    -DXSDK_TOOLCHAIN="$XSDK_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build "$BUILD_DIR" --parallel

XEX="$BUILD_DIR/padcheck360.xex"
[[ -s "$XEX" ]] || fail "A compilacao terminou, mas $XEX nao foi criado. Confira as mensagens acima."
printf '\nSUCESSO: XEX criado em:\n%s\n' "$XEX"
printf 'Copie esse arquivo para um pendrive FAT32 ou para o HD do Xbox 360 e abra pela Aurora/XeXMenu.\n'
