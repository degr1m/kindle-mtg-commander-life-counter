#!/bin/sh
set -eu

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
VM_NAME=${KINDLE_BUILD_VM:-kindle-builder}
SOURCE="$ROOT_DIR/native"
OUTPUT="$ROOT_DIR/extensions/mtg-life-counter/bin/mtg-life-counter"

limactl shell "$VM_NAME" -- bash -lc "
  set -eu
  TOOLCHAIN=\"\$HOME/x-tools/arm-kindlehf-linux-gnueabihf\"
  BUILD=\"\$HOME/mtg-life-counter-build\"
  rm -rf \"\$BUILD\"
  meson setup \"\$BUILD\" '$SOURCE' --cross-file=\"\$TOOLCHAIN/meson-crosscompile.txt\"
  meson compile -C \"\$BUILD\"
  \"\$TOOLCHAIN/bin/arm-kindlehf-linux-gnueabihf-strip\" \"\$BUILD/mtg-life-counter\"
  cp \"\$BUILD/mtg-life-counter\" '$OUTPUT'
  file '$OUTPUT'
"
