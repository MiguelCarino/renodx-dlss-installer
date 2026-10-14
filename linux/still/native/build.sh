#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Rebuild bin/nrstill.exe and bin/nvngx.dll_nrshim.dll with zig (x86_64-windows-gnu).
# Needs zig (or `pip install ziglang` into a venv and ZIG="<venv>/bin/python -m ziglang").
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/../bin
ZIG=${ZIG:-zig}
mkdir -p "$OUT"
# -fno-optimize-sibling-calls: the shim must CALL the snippet, never tail-jump into it,
# or the snippet sees the exe as its caller and refuses (see nrshim.cpp).
$ZIG c++ -target x86_64-windows-gnu -O1 -fno-optimize-sibling-calls -std=c++17 -shared \
  -o "$OUT/nvngx.dll_nrshim.dll" "$HERE/nrshim.cpp"
$ZIG c++ -target x86_64-windows-gnu -O2 -std=c++17 -static \
  -o "$OUT/nrstill.exe" "$HERE/nrstill.cpp" -ld3d12 -ldxgi -ldxguid -luser32
rm -f "$OUT"/*.pdb "$OUT"/*.lib
echo built
