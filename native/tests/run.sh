#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GAME_OUT=$(mktemp -t mtg-game-test)
LAYOUT_OUT=$(mktemp -t mtg-layout-test)
DEVICE_OUT=$(mktemp -t mtg-device-test)
trap 'rm -f "$GAME_OUT" "$LAYOUT_OUT" "$DEVICE_OUT"' EXIT

${CC:-cc} -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
  -I"$ROOT/native/src" \
  "$ROOT/native/tests/test_game.c" \
  "$ROOT/native/src/game.c" \
  -o "$GAME_OUT"
"$GAME_OUT"

${CC:-cc} -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
  -I"$ROOT/native/src" \
  "$ROOT/native/tests/test_layout.c" \
  "$ROOT/native/src/game.c" \
  "$ROOT/native/src/layout.c" \
  -lm \
  -o "$LAYOUT_OUT"
"$LAYOUT_OUT"

${CC:-cc} -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
  -I"$ROOT/native/src" \
  "$ROOT/native/tests/test_device.c" \
  "$ROOT/native/src/device.c" \
  -o "$DEVICE_OUT"
"$DEVICE_OUT"
