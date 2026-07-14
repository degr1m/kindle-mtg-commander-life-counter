#!/bin/sh
set -u

APP_DIR=${MTG_APP_DIR:-/mnt/us/extensions/mtg-life-counter}
BIN="$APP_DIR/bin/mtg-life-counter"
DATA_DIR="$APP_DIR/data"
STATE_FILE="$DATA_DIR/state.txt"
LOCK_DIR="$DATA_DIR/app.lock"
LOCK_GUARD="$DATA_DIR/app.lock.guard"
INSTALL_LOCK="$DATA_DIR/install.lock"
LOG_FILE="$APP_DIR/app.log"
child_pid=
child_reaped=0
lock_owned=0
guard_owned=0
grace_steps=${MTG_SHUTDOWN_GRACE_STEPS:-5}

case "$grace_steps" in
  '' | *[!0-9]*) grace_steps=5 ;;
esac

mkdir -p "$DATA_DIR" 2>/dev/null || exit 1

valid_pid() {
  case "${1:-}" in
    '' | 0 | 0* | *[!0-9]*) return 1 ;;
  esac
  return 0
}

lock_pid_from() {
  sed -n '1p' "$1/pid" 2>/dev/null || true
}

release_guard() {
  [ "$guard_owned" = 1 ] || return 0
  owner=$(lock_pid_from "$LOCK_GUARD")
  if [ "$owner" = "$$" ]; then
    rm -f "$LOCK_GUARD/pid" 2>/dev/null || true
    rmdir "$LOCK_GUARD" 2>/dev/null || true
  fi
  guard_owned=0
}

acquire_guard() {
  candidate="$DATA_DIR/.app.lock.guard.$$"
  stale="$DATA_DIR/.app.lock.guard.stale.$$"
  rm -rf "$candidate" "$stale" 2>/dev/null || true
  mkdir "$candidate" 2>/dev/null || return 1
  if ! printf '%s\n' "$$" >"$candidate/pid"; then
    rm -rf "$candidate" 2>/dev/null || true
    return 1
  fi

  if [ -e "$LOCK_GUARD" ]; then
    observed=$(lock_pid_from "$LOCK_GUARD")
    if ! valid_pid "$observed" || kill -0 "$observed" 2>/dev/null; then
      rm -rf "$candidate" 2>/dev/null || true
      return 1
    fi
    if ! mv "$LOCK_GUARD" "$stale" 2>/dev/null; then
      rm -rf "$candidate" 2>/dev/null || true
      return 1
    fi
    moved=$(lock_pid_from "$stale")
    if [ "$moved" != "$observed" ]; then
      [ -e "$LOCK_GUARD" ] || mv "$stale" "$LOCK_GUARD" 2>/dev/null || true
      rm -rf "$candidate" 2>/dev/null || true
      return 1
    fi
    rm -rf "$stale" 2>/dev/null || true
  fi

  mv "$candidate" "$LOCK_GUARD" 2>/dev/null || true
  owner=$(lock_pid_from "$LOCK_GUARD")
  if [ "$owner" != "$$" ]; then
    rm -rf "$LOCK_GUARD/.app.lock.guard.$$" "$candidate" 2>/dev/null || true
    return 1
  fi
  guard_owned=1
  return 0
}

release_lock() {
  [ "$lock_owned" = 1 ] || return 0
  owner=$(lock_pid_from "$LOCK_DIR")
  if [ "$owner" = "$$" ]; then
    rm -f "$LOCK_DIR/pid" 2>/dev/null || true
    rmdir "$LOCK_DIR" 2>/dev/null || true
  fi
  lock_owned=0
}

acquire_lock() {
  acquire_guard || return 1
  if [ -e "$INSTALL_LOCK" ]; then
    release_guard
    return 1
  fi
  lock_pid=$(lock_pid_from "$LOCK_DIR")
  if valid_pid "$lock_pid" && kill -0 "$lock_pid" 2>/dev/null; then
    release_guard
    return 1
  fi
  rm -rf "$LOCK_DIR" 2>/dev/null || {
    release_guard
    return 1
  }
  if ! mkdir "$LOCK_DIR" 2>/dev/null ||
     ! printf '%s\n' "$$" >"$LOCK_DIR/pid"; then
    rm -rf "$LOCK_DIR" 2>/dev/null || true
    release_guard
    return 1
  fi
  lock_owned=1
  release_guard
  return 0
}

if ! acquire_lock; then
  printf '%s\n' "MTG Commander Life Counter is already running." >>"$LOG_FILE" 2>/dev/null
  exit 1
fi

chmod +x "$BIN" 2>/dev/null || true
if [ ! -x "$BIN" ]; then
  printf '%s\n' "Missing executable: $BIN" >>"$LOG_FILE" 2>/dev/null
  release_lock
  exit 1
fi

if [ -f "$LOG_FILE" ] && [ "$(wc -c <"$LOG_FILE" 2>/dev/null || printf 0)" -gt 131072 ]; then
  mv "$LOG_FILE" "$LOG_FILE.old" 2>/dev/null || true
fi

child_group_alive() {
  [ -n "$child_pid" ] && kill -0 "-$child_pid" 2>/dev/null
}

terminate_child() {
  [ -n "$child_pid" ] || return 0
  if ! kill -TERM "-$child_pid" 2>/dev/null && [ "$child_reaped" = 0 ]; then
    kill -TERM "$child_pid" 2>/dev/null || true
  fi
  remaining=$grace_steps
  while child_group_alive && [ "$remaining" -gt 0 ]; do
    sleep 1
    remaining=$((remaining - 1))
  done
  if child_group_alive; then
    kill -KILL "-$child_pid" 2>/dev/null || true
  elif [ "$child_reaped" = 0 ]; then
    kill -KILL "$child_pid" 2>/dev/null || true
  fi
  if [ "$child_reaped" = 0 ]; then
    wait "$child_pid" 2>/dev/null || true
  fi
  child_pid=
  child_reaped=0
}

restore() {
  status=$?
  trap - EXIT HUP INT TERM
  terminate_child
  release_lock
  release_guard
  exit "$status"
}

trap restore EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

export DISPLAY=:0
printf '%s launching native commander life counter\n' \
  "$(date '+%Y-%m-%d %H:%M:%S')" >>"$LOG_FILE" 2>&1
"$BIN" "$STATE_FILE" >>"$LOG_FILE" 2>&1 &
child_pid=$!
wait "$child_pid"
status=$?
child_reaped=1
printf '%s native app exited with status %s\n' \
  "$(date '+%Y-%m-%d %H:%M:%S')" "$status" >>"$LOG_FILE" 2>&1
exit "$status"
