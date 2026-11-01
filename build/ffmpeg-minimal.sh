#!/bin/bash
# ffmpeg-minimal.sh - build the FFmpeg the XAVC decoder ships with (run in WSL / Ubuntu 24.04).
#
# libavutil + libavcodec with only the H.264 decoder (x86 assembly enabled) and the NVDEC
# hwaccel, as shared DLLs named avcodec-xavc-NN.dll / avutil-xavc-NN.dll, so they never clash
# with another FFmpeg loaded in the same process. No external libraries: LGPL 2.1 or later.
#
#   sudo apt-get install -y gcc-mingw-w64-x86-64 nasm make pkg-config git python3
#   bash build/ffmpeg-minimal.sh [<FFmpeg commit>] [<output dir>]
#
# Then: build\build-win.ps1 -FFmpeg <output dir>
set -euo pipefail
COMMIT=${1:-4811625b5f4e5aed528aed9225d0ca868a8e6e96}   # FFmpeg master, 2026-10-31
OUT=${2:-/mnt/e/TQN_Projects/_toolchains/ffmpeg/ffmin-xavc}
WORK=$HOME/ffbuild
mkdir -p "$WORK" && cd "$WORK"
if [ ! -d FFmpeg/.git ]; then
  git init -q FFmpeg
  git -C FFmpeg remote add origin https://github.com/FFmpeg/FFmpeg.git
fi
git -C FFmpeg fetch -q --depth 1 origin "$COMMIT"
git -C FFmpeg checkout -q FETCH_HEAD
[ -d nv-codec-headers/.git ] || git clone -q --depth 1 https://github.com/FFmpeg/nv-codec-headers.git
make -C nv-codec-headers PREFIX="$WORK/nv" install > /dev/null
PREFIX=$WORK/out
rm -rf build "$PREFIX" && mkdir build && cd build
export PKG_CONFIG_PATH=$WORK/nv/lib/pkgconfig
../FFmpeg/configure \
  --prefix="$PREFIX" \
  --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32- --enable-cross-compile \
  --pkg-config=pkg-config \
  --enable-shared --disable-static --build-suffix=-xavc \
  --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything \
  --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-decoder=h264 --enable-parser=h264 \
  --enable-w32threads \
  --enable-ffnvcodec --enable-nvdec --enable-hwaccel=h264_nvdec \
  --disable-debug --extra-ldflags=-static-libgcc > configure.log
grep -E "^(x86 assembler|threading support|License|Enabled (decoders|hwaccels)|External libraries)" -A1 configure.log | head -20
make -j"$(nproc)" > make.log 2>&1 || { tail -30 make.log; exit 1; }
make install > /dev/null
ls -la "$PREFIX/bin"
for f in "$PREFIX"/bin/*.dll; do echo "$(basename $f): $(x86_64-w64-mingw32-objdump -p $f | grep 'DLL Name' | awk '{print $3}' | tr '\n' ' ')"; done

rm -rf "$OUT" && mkdir -p "$OUT"
cp -r "$PREFIX/bin" "$PREFIX/include" "$PREFIX/lib" "$OUT/"
cp ../FFmpeg/COPYING.LGPLv2.1 ../FFmpeg/LICENSE.md "$OUT/"
git -C ../FFmpeg log -1 --format='%H' > "$OUT/FFMPEG_COMMIT.txt"
cp configure.log "$OUT/"
echo "copied to $OUT"
