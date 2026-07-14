#!/bin/sh
set -eu

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
RELEASE_TREE="$ROOT_DIR/scripts/release_tree.py"
STAGING=$(mktemp -d "${TMPDIR:-/tmp}/mtg-life-counter-install.XXXXXX")
VOLUME_ACTIVE=0
VOLUME_TARGET=
VOLUME_INCOMING=
VOLUME_BACKUP=
VOLUME_COMMIT=
VOLUME_HAD_TARGET=0
VOLUME_CUTOVER=0
VOLUME_COMMITTED=0
VOLUME_LOCK_OWNED=0
VOLUME_DATA_CREATED=0
VOLUME_GUARD_OWNED=0
VOLUME_HOST_GUARD=
VOLUME_HOST_GUARD_OWNED=0

release_volume_lock() {
  [ "$VOLUME_LOCK_OWNED" = 1 ] || return 0
  for root in "$VOLUME_TARGET" "$VOLUME_BACKUP"; do
    [ -n "$root" ] || continue
    lock="$root/data/install.lock"
    owner=$(sed -n '1p' "$lock/owner" 2>/dev/null || true)
    if [ "$owner" = "$$" ]; then
      rm -f "$lock/owner" 2>/dev/null || true
      rmdir "$lock" 2>/dev/null || true
    fi
  done
  if [ "$VOLUME_DATA_CREATED" = 1 ]; then
    rmdir "$VOLUME_TARGET/data" 2>/dev/null || true
  fi
  VOLUME_LOCK_OWNED=0
}

release_volume_guard() {
  [ "$VOLUME_GUARD_OWNED" = 1 ] || return 0
  for root in "$VOLUME_TARGET" "$VOLUME_BACKUP"; do
    [ -n "$root" ] || continue
    guard="$root/data/app.lock.guard"
    owner=$(sed -n '1p' "$guard/installer-owner" 2>/dev/null || true)
    if [ "$owner" = "$$" ]; then
      rm -f "$guard/pid" "$guard/installer-owner" 2>/dev/null || true
      rmdir "$guard" 2>/dev/null || true
    fi
  done
  VOLUME_GUARD_OWNED=0
}

release_volume_host_guard() {
  [ "$VOLUME_HOST_GUARD_OWNED" = 1 ] || return 0
  owner=$(sed -n '1p' "$VOLUME_HOST_GUARD/owner" 2>/dev/null || true)
  if [ "$owner" = "$$" ]; then
    rm -f "$VOLUME_HOST_GUARD/owner" 2>/dev/null || true
    rmdir "$VOLUME_HOST_GUARD" 2>/dev/null || true
  elif [ -z "$owner" ]; then
    rmdir "$VOLUME_HOST_GUARD" 2>/dev/null || true
  fi
  VOLUME_HOST_GUARD_OWNED=0
}

acquire_volume_host_guard() {
  guard="$1"
  if ! mkdir "$guard" 2>/dev/null; then
    [ -d "$guard" ] && [ ! -L "$guard" ] || return 1
    observed=$(sed -n '1p' "$guard/owner" 2>/dev/null || true)
    case "$observed" in
      '' | 0 | 0* | *[!0-9]*) return 1 ;;
    esac
    if kill -0 "$observed" 2>/dev/null; then return 1; fi
    current=$(sed -n '1p' "$guard/owner" 2>/dev/null || true)
    [ "$current" = "$observed" ] || return 1
    if kill -0 "$current" 2>/dev/null; then return 1; fi
    rm -f "$guard/owner" 2>/dev/null || return 1
    rmdir "$guard" 2>/dev/null || return 1
    mkdir "$guard" 2>/dev/null || return 1
  fi
  VOLUME_HOST_GUARD="$guard"
  VOLUME_HOST_GUARD_OWNED=1
  printf '%s\n' "$$" >"$guard/owner" || return 1
  owner=$(sed -n '1p' "$guard/owner" 2>/dev/null || true)
  [ "$owner" = "$$" ] || return 1
}

reclaim_stale_volume_lock() {
  lock="$1"
  if [ ! -e "$lock" ] && [ ! -L "$lock" ]; then return 0; fi
  [ -d "$lock" ] && [ ! -L "$lock" ] || {
    echo "Refusing invalid mounted-volume installer lock." >&2
    return 1
  }
  owner=$(sed -n '1p' "$lock/owner" 2>/dev/null || true)
  case "$owner" in
    '' | 0 | 0* | *[!0-9]*) owner= ;;
  esac
  if [ -n "$owner" ] && kill -0 "$owner" 2>/dev/null; then
    echo "Another mounted-volume installer is running." >&2
    return 1
  fi
  rm -rf "$lock"
}

reclaim_stale_volume_guard() {
  guard="$1"
  if [ ! -e "$guard" ] && [ ! -L "$guard" ]; then return 0; fi
  [ -d "$guard" ] && [ ! -L "$guard" ] || return 1
  pid=$(sed -n '1p' "$guard/pid" 2>/dev/null || true)
  owner=$(sed -n '1p' "$guard/installer-owner" 2>/dev/null || true)
  case "$owner" in
    '' | 0 | 0* | *[!0-9]*) return 1 ;;
  esac
  [ "$pid" = 1 ] || return 1
  if kill -0 "$owner" 2>/dev/null; then return 1; fi
  current=$(sed -n '1p' "$guard/installer-owner" 2>/dev/null || true)
  [ "$current" = "$owner" ] || return 1
  rm -f "$guard/pid" "$guard/installer-owner" 2>/dev/null || return 1
  rmdir "$guard" 2>/dev/null || return 1
}

cleanup() {
  if [ "$VOLUME_ACTIVE" = 1 ]; then
    if [ "$VOLUME_COMMITTED" = 1 ] || [ -d "$VOLUME_COMMIT" ]; then
      rm -rf "$VOLUME_INCOMING" 2>/dev/null || true
    elif [ -d "$VOLUME_BACKUP" ]; then
      rm -rf "$VOLUME_TARGET"
      mv "$VOLUME_BACKUP" "$VOLUME_TARGET" 2>/dev/null || true
    elif [ "$VOLUME_HAD_TARGET" = 0 ] && [ "$VOLUME_CUTOVER" = 1 ]; then
      rm -rf "$VOLUME_TARGET"
    fi
    rm -rf "$VOLUME_INCOMING"
  fi
  release_volume_guard
  release_volume_lock
  release_volume_host_guard
  rm -rf "$STAGING"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
SOURCE="$STAGING/mtg-life-counter"
python3 "$RELEASE_TREE" stage \
  "$ROOT_DIR/extensions/mtg-life-counter" "$SOURCE"

install_volume() {
  kindle_root="$1"
  [ -d "$kindle_root" ] && [ ! -L "$kindle_root" ] || {
    echo "Kindle root does not exist: $kindle_root" >&2
    exit 1
  }
  extensions="$kindle_root/extensions"
  target="$extensions/mtg-life-counter"
  incoming="$extensions/.mtg-life-counter.new"
  backup="$extensions/.mtg-life-counter.old"
  commit="$extensions/.mtg-life-counter.commit"
  host_guard="$extensions/.mtg-life-counter.installer"
  VOLUME_TARGET="$target"
  VOLUME_INCOMING="$incoming"
  VOLUME_BACKUP="$backup"
  VOLUME_COMMIT="$commit"
  if [ -e "$extensions" ] || [ -L "$extensions" ]; then
    [ -d "$extensions" ] && [ ! -L "$extensions" ] || {
      echo "Refusing non-directory or symlinked extensions path." >&2
      exit 1
    }
  else
    mkdir "$extensions"
  fi

  for path in "$target" "$incoming" "$backup" "$commit" "$host_guard"; do
    if [ -e "$path" ] || [ -L "$path" ]; then
      [ -d "$path" ] && [ ! -L "$path" ] || {
        echo "Refusing special, non-directory, or symlinked transaction path." >&2
        exit 1
      }
    fi
  done
  acquire_volume_host_guard "$host_guard" || {
    echo "Another mounted-volume installer is active or left an invalid guard." >&2
    exit 1
  }

  if [ -e "$target/data" ] || [ -L "$target/data" ]; then
    python3 "$RELEASE_TREE" validate-data "$target/data"
  fi
  if [ -e "$backup/data" ] || [ -L "$backup/data" ]; then
    python3 "$RELEASE_TREE" validate-data "$backup/data"
  fi
  reclaim_stale_volume_guard "$target/data/app.lock.guard"
  reclaim_stale_volume_guard "$backup/data/app.lock.guard"
  for root in "$target" "$backup"; do
    [ -d "$root" ] || continue
    if [ ! -d "$root/data" ]; then
      mkdir "$root/data"
      if [ "$root" = "$target" ]; then VOLUME_DATA_CREATED=1; fi
    fi
    mkdir "$root/data/app.lock.guard"
    VOLUME_GUARD_OWNED=1
    printf '%s\n' "$$" >"$root/data/app.lock.guard/installer-owner"
    printf '%s\n' 1 >"$root/data/app.lock.guard/pid"
  done
  reclaim_stale_volume_lock "$target/data/install.lock"
  reclaim_stale_volume_lock "$backup/data/install.lock"
  for root in "$target" "$backup"; do
    [ -d "$root" ] || continue
    if [ ! -d "$root/data" ]; then mkdir "$root/data"; fi
    mkdir "$root/data/install.lock"
    VOLUME_LOCK_OWNED=1
    printf '%s\n' "$$" >"$root/data/install.lock/owner"
  done
  if [ -e "$target/data/app.lock" ]; then
    echo "Life counter started during installer lock publication." >&2
    exit 1
  fi

  verify_code() {
    candidate="$1"
    cmp "$SOURCE/bin/launch.sh" "$candidate/bin/launch.sh" >/dev/null &&
      cmp "$SOURCE/bin/mtg-life-counter" \
        "$candidate/bin/mtg-life-counter" >/dev/null &&
      cmp "$SOURCE/config.xml" "$candidate/config.xml" >/dev/null &&
      cmp "$SOURCE/menu.json" "$candidate/menu.json" >/dev/null
  }

  if [ -d "$commit" ]; then
    verify_code "$target" || {
      echo "Committed mounted-volume target failed verification." >&2
      exit 1
    }
    rm -rf "$incoming" "$backup"
    rmdir "$commit"
  elif [ -d "$backup" ]; then
    for relative in bin/launch.sh bin/mtg-life-counter config.xml menu.json; do
      [ -f "$backup/$relative" ] && [ ! -L "$backup/$relative" ] || {
        echo "Previous mounted-volume backup is incomplete." >&2
        exit 1
      }
    done
    if [ -e "$backup/data" ] || [ -L "$backup/data" ]; then
      python3 "$RELEASE_TREE" validate-data "$backup/data"
    fi
    rm -rf "$target"
    if ! mv "$backup" "$target"; then
      [ ! -e "$backup" ] && [ -d "$target" ] || {
        echo "Could not recover the previous mounted-volume installation." >&2
        exit 1
      }
    fi
  fi
  rm -rf "$incoming"

  if [ -e "$target/data" ] || [ -L "$target/data" ]; then
    python3 "$RELEASE_TREE" validate-data "$target/data"
  fi

  VOLUME_ACTIVE=1
  VOLUME_TARGET="$target"
  VOLUME_INCOMING="$incoming"
  VOLUME_BACKUP="$backup"
  VOLUME_COMMIT="$commit"
  if [ -d "$target" ]; then VOLUME_HAD_TARGET=1; fi
  cp -R "$SOURCE" "$incoming"

  if [ -e "$target/data" ]; then
    cp -R "$target/data" "$incoming/data"
    if ! diff -qr "$target/data" "$incoming/data" >/dev/null; then
      echo "Could not verify preserved game data." >&2
      rm -rf "$incoming"
      exit 1
    fi
  fi

  if [ -d "$target" ]; then
    target_owner=$(sed -n '1p' "$target/data/install.lock/owner" 2>/dev/null || true)
    incoming_owner=$(sed -n '1p' "$incoming/data/install.lock/owner" 2>/dev/null || true)
    [ "$target_owner" = "$$" ] && [ "$incoming_owner" = "$$" ] || {
      echo "Mounted-volume installer lock ownership changed." >&2
      exit 1
    }
  else
    mkdir -p "$incoming/data"
    mkdir "$incoming/data/install.lock"
    printf '%s\n' "$$" >"$incoming/data/install.lock/owner"
    VOLUME_LOCK_OWNED=1
  fi

  if [ -e "$target/data/app.lock" ]; then
    echo "Life counter started during staging; installation cancelled." >&2
    exit 1
  fi

  if [ -e "$target" ]; then
    mv "$target" "$backup"
    VOLUME_CUTOVER=1
    if ! mv "$incoming" "$target"; then
      echo "Install cutover failed; previous installation restored." >&2
      exit 1
    fi
  else
    VOLUME_CUTOVER=1
    mv "$incoming" "$target"
  fi

  verify_code "$target"
  if [ -d "$backup/data" ] &&
     ! diff -qr "$backup/data" "$target/data" >/dev/null; then
    echo "Preserved game data changed during mounted-volume cutover." >&2
    exit 1
  fi
  if ! mkdir "$commit"; then
    [ -d "$commit" ] || exit 1
  fi
  VOLUME_COMMITTED=1
  release_volume_guard
  release_volume_lock
  rm -rf "$backup"
  rmdir "$commit"
  VOLUME_ACTIVE=0
  echo "Installed to $target"
}

install_mtp() {
  prefix=$(brew --prefix libmtp 2>/dev/null || true)
  [ -n "$prefix" ] || {
    echo "MTP support is missing. Install it with: brew install libmtp" >&2
    exit 1
  }
  installer="$STAGING/mtp-installer"
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
