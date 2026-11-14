#!/usr/bin/env bash
# Device builds without Docker: zig (pip install ziglang) cross-compiles the plugin for
#   armv7   (Gen1: Force, MPC Live/One/X; glibc 2.27 floor) -> vst/build/m-one.so
#   aarch64 (Gen2: MPC XL, Live III ...;    glibc 2.27 floor) -> vst/build/aarch64/m-one.so
# Needs: vst/build/params.h (gen_vst.py, which also builds the skin). No ROMs: the plugin reads them at start-up.
#   tools/build_zig.sh <path to open-mpc mpc-vst-plugins checkout> [armv7|aarch64|all]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; MV="$(cd "$1" && pwd)"; WHICH="${2:-all}"
ZIG="python3 -m ziglang"
[ -f "$HERE/vst/build/params.h" ] || { echo "run gen_vst.py vst/vst.json first"; exit 1; }
build() {   # build <zig target> <cpu flags> <out dir>
  local T="$1" CPU="$2" OUT="$3" O; mkdir -p "$OUT/obj"
  local COMMON="-target $T $CPU -O2 -fPIC -fvisibility=hidden -I$HERE/vst/build -I$MV/wrapper -I$HERE/src/m1/include"
  local OBJS=""
  for f in src/mone.cpp src/m1_romload.cpp src/m1/src/m1_engine.cpp src/m1/src/m1_fx.cpp; do
    O="$OUT/obj/$(basename "$f").o"
    $ZIG c++ $COMMON -std=gnu++17 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter -Wno-missing-field-initializers -c "$HERE/$f" -o "$O"
    OBJS="$OBJS $O"
  done
  $ZIG cc $COMMON -std=gnu11 -D_GNU_SOURCE -c "$MV/wrapper/vst2_wrap.c" -o "$OUT/obj/vst2_wrap.o"
  $ZIG c++ -target $T $CPU -shared -fPIC $OBJS "$OUT/obj/vst2_wrap.o" -lm -lpthread -ldl \
      -Wl,--no-undefined -Wl,--version-script=$HERE/src/exports.map -s -o "$OUT/m-one.so"
  echo "$T: $(md5sum < "$OUT/m-one.so" | cut -d' ' -f1)  $(stat -c %s "$OUT/m-one.so") bytes"
}
case "$WHICH" in armv7|all) build arm-linux-gnueabihf.2.27 "-mcpu=cortex_a7" "$HERE/vst/build" ;; esac
case "$WHICH" in aarch64|all) build aarch64-linux-gnu.2.27 "-mcpu=cortex_a53" "$HERE/vst/build/aarch64" ;; esac
