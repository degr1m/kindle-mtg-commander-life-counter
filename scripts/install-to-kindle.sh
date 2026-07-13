#!/bin/sh
set -eu

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
SOURCE="$ROOT_DIR/extensions/mtg-life-counter"

install_volume() {
  kindle_root="$1"
  [ -d "$kindle_root" ] || {
    echo "Kindle root does not exist: $kindle_root" >&2
    exit 1
  }
  mkdir -p "$kindle_root/extensions"
  rm -rf "$kindle_root/extensions/mtg-life-counter"
  cp -R "$SOURCE" "$kindle_root/extensions/"
  echo "Installed to $kindle_root/extensions/mtg-life-counter"
}

install_mtp() {
  prefix=$(brew --prefix libmtp 2>/dev/null || true)
  [ -n "$prefix" ] || {
    echo "MTP support is missing. Install it with: brew install libmtp" >&2
    exit 1
  }
  installer=$(mktemp -t mtg-life-counter-mtp-install)
  trap 'rm -f "$installer"' EXIT HUP INT TERM
  clang -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror \
    -I"$prefix/include" -L"$prefix/lib" -Wl,-rpath,"$prefix/lib" \
    "$ROOT_DIR/scripts/mtp-install.c" -lmtp -o "$installer"
  "$installer" "$SOURCE"
}

if [ "${1:-}" ]; then
  install_volume "$1"
elif [ -d /Volumes/Kindle ]; then
  install_volume /Volumes/Kindle
else
  echo "No mounted Kindle volume found; using MTP."
  echo "Keep the Kindle connected and unlocked until INSTALL_COMPLETE appears."
  install_mtp
fi
