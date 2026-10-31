#!/bin/bash
# Cross-compile the DIAGNOSTIC XAVC decoder MFT (xavc_decoder.dll) on Linux using
# the system clang + lld and an llvm-mingw sysroot (headers/libs used as DATA; the
# mingw compiler itself is NOT executed).
#
#   MG=/path/to/llvm-mingw-...-x86_64  ./build/build-diag.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${MG:?set MG to the extracted llvm-mingw toolchain directory}"
OUT="${OUT:-$ROOT/build/out}"
mkdir -p "$OUT"

TGT=x86_64-w64-mingw32
SYS="$MG/$TGT"
RES="$MG/lib/clang/18"

CXX=(clang --target=$TGT --sysroot="$SYS" -resource-dir="$RES"
     -rtlib=compiler-rt --unwindlib=none -fuse-ld=lld
     -I"$SYS/include" -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
     -O2 -fno-exceptions -fno-rtti -Wall -Wno-dll-attribute-on-redeclaration)

echo "[1/3] compile guids.cpp"
"${CXX[@]}" -c "$ROOT/src/guids.cpp"          -o "$OUT/guids.o"
echo "[2/3] compile xavc_diag_mft.cpp"
"${CXX[@]}" -c "$ROOT/src/xavc_diag_mft.cpp"  -o "$OUT/mft.o"
echo "[3/3] link xavc_decoder.dll"
"${CXX[@]}" -shared -o "$OUT/xavc_decoder.dll" \
    "$OUT/guids.o" "$OUT/mft.o" "$ROOT/src/xavc.def" \
    -L"$SYS/lib" -lmfplat -lmfuuid -lmf -lole32 -loleaut32 -luuid

echo "OK -> $OUT/xavc_decoder.dll"
