#!/usr/bin/env bash
# The open-mpc host test (tools/host_test.c) built for ARM with zig (static musl) and run under qemu-user:
#   MPC_PLUGIN_FOLDER=<folder with plugin.json + Content/ROMs> tools/test_arm.sh <mpc-vst-plugins checkout> [armv7|aarch64|all]      (needs qemu-arm / qemu-aarch64)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; MV="$(cd "$1" && pwd)"; WHICH="${2:-all}"; ZIG="python3 -m ziglang"
run() {   # run <zig target> <cpu> <qemu>
  local T="$1" CPU="$2" Q="$3" D="$HERE/vst/build/test-$1"; mkdir -p "$D"
  local C="-target $T $CPU -O1 -g -I$HERE/vst/build -I$MV/wrapper -I$HERE/src/m1/include"
  local OBJS=""
  for f in src/mone.cpp src/m1_romload.cpp src/m1/src/m1_engine.cpp src/m1/src/m1_fx.cpp; do
    $ZIG c++ $C -std=gnu++17 -fno-exceptions -fno-rtti -w -c "$HERE/$f" -o "$D/$(basename $f).o"; OBJS="$OBJS $D/$(basename $f).o"; done
  for f in "$MV/tools/host_test.c" "$MV/wrapper/vst2_wrap.c"; do
    $ZIG cc $C -std=gnu11 -D_GNU_SOURCE -w -c "$f" -o "$D/$(basename $f).o"; OBJS="$OBJS $D/$(basename $f).o"; done
  $ZIG c++ -target $T $CPU -static $OBJS -lm -o "$D/host_test"
  echo "== $T under $Q"; $Q "$D/host_test" | grep -v '^ok ' | tail -n 6
}
case "$WHICH" in armv7|all) run arm-linux-musleabihf "-mcpu=cortex_a7" qemu-arm ;; esac
case "$WHICH" in aarch64|all) run aarch64-linux-musl "-mcpu=cortex_a53" qemu-aarch64 ;; esac
