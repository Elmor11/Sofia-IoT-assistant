#!/usr/bin/env bash
# Compilacion rapida en Windows + Docker: el codigo se copia a un volumen de Docker
# (el disco montado de Windows es lo que hace lenta la compilacion) y se usa ccache.
# Uso (desde la carpeta del proyecto, en Git Bash):
#   scripts/build-fast.sh [placa] [salida]
# La primera vez tarda como una compilacion normal; las siguientes reutilizan todo.
set -e
BOARD="${1:-bread-compact-esp32}"
OUT="${2:-$HOME/Downloads/xiaozhi-build-out}"
SRC="$(pwd -W)"
mkdir -p "$OUT"

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$SRC:/src:ro" \
  -v sofia-work:/work \
  -v sofia-ccache:/ccache \
  -v "$(cd "$OUT" && pwd -W):/out" \
  -e IDF_CCACHE_ENABLE=1 -e CCACHE_DIR=/ccache \
  espressif/idf:release-v6.1 bash -c '
set -e
mkdir -p /work
cd /src
# Copia el codigo, sin build ni .git (build y components se conservan en el volumen)
tar cf - --exclude=./build --exclude=./.git --exclude=./sdkconfig --exclude=./sdkconfig.old . | (cd /work && tar xf -)
cd /work
python3 scripts/build.py '"$BOARD"' --name '"$BOARD"'
cp -f build/merged-binary.bin /out/merged-binary.bin
cp -f build/xiaozhi.bin /out/xiaozhi.bin
ccache -s | head -8
echo BUILD_OK
'
