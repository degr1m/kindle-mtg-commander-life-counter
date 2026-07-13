#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -t mtg-game-test)
trap 'rm -f "$OUT"' EXIT

${CC:-cc} -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
  -I"$ROOT/native/src" \
  "$ROOT/native/tests/test_game.c" \
  "$ROOT/native/src/game.c" \
  -o "$OUT"
"$OUT"
