#!/bin/sh
set -eu

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${1:-2.0.0}
SOURCE="$ROOT_DIR/extensions/mtg-life-counter"
DIST_DIR="$ROOT_DIR/dist"
ARCHIVE="$DIST_DIR/mtg-life-counter-v${VERSION}-kindle.zip"
RELEASE_TREE="$ROOT_DIR/scripts/release_tree.py"

case "$VERSION" in
  *[!0-9.]* | .* | *.)
    echo "Invalid version: $VERSION" >&2
    exit 2
    ;;
esac

python3 "$RELEASE_TREE" package "$SOURCE" "$ARCHIVE"
unzip -tq "$ARCHIVE"

printf 'PACKAGE_COMPLETE %s\n' "$ARCHIVE"
shasum -a 256 "$ARCHIVE"
