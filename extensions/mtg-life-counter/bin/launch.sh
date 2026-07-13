#!/bin/sh
set -u

APP_DIR=/mnt/us/extensions/mtg-life-counter
BIN="$APP_DIR/bin/mtg-life-counter"
DATA_DIR="$APP_DIR/data"
STATE_FILE="$DATA_DIR/state.txt"
LOCK_DIR="$DATA_DIR/app.lock"
LOG_FILE="$APP_DIR/app.log"
child_pid=
power_managed=0
previous_power=

mkdir -p "$DATA_DIR" 2>/dev/null || exit 1

acquire_lock() {
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    printf '%s\n' "$$" >"$LOCK_DIR/pid"
    return 0
  fi
  lock_pid=$(sed -n '1p' "$LOCK_DIR/pid" 2>/dev/null || true)
  if [ -n "$lock_pid" ] && kill -0 "$lock_pid" 2>/dev/null; then
    return 1
  fi
  rm -f "$LOCK_DIR/pid" 2>/dev/null || true
  rmdir "$LOCK_DIR" 2>/dev/null || return 1
  mkdir "$LOCK_DIR" 2>/dev/null || return 1
  printf '%s\n' "$$" >"$LOCK_DIR/pid"
}

if ! acquire_lock; then
  printf '%s\n' "MTG Commander Life Counter is already running." >>"$LOG_FILE" 2>/dev/null
  exit 1
fi

chmod +x "$BIN" 2>/dev/null || true
if [ ! -x "$BIN" ]; then
  printf '%s\n' "Missing executable: $BIN" >>"$LOG_FILE" 2>/dev/null
  rm -f "$LOCK_DIR/pid" 2>/dev/null || true
  rmdir "$LOCK_DIR" 2>/dev/null || true
  exit 1
fi

if [ -f "$LOG_FILE" ] && [ "$(wc -c <"$LOG_FILE" 2>/dev/null || printf 0)" -gt 131072 ]; then
  mv "$LOG_FILE" "$LOG_FILE.old" 2>/dev/null || true
fi

restore() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ -n "$child_pid" ] && kill -0 "$child_pid" 2>/dev/null; then
    kill -TERM "$child_pid" 2>/dev/null || true
    wait "$child_pid" 2>/dev/null || true
  fi
  if [ "$power_managed" -eq 1 ]; then
    lipc-set-prop com.lab126.powerd preventScreenSaver "$previous_power" \
      >/dev/null 2>&1 || true
  fi
  rm -f "$LOCK_DIR/pid" 2>/dev/null || true
  rmdir "$LOCK_DIR" 2>/dev/null || true
  exit "$status"
}

trap restore EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if previous_power=$(lipc-get-prop com.lab126.powerd preventScreenSaver 2>/dev/null); then
  if lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1; then
    power_managed=1
  fi
fi

export DISPLAY=:0
printf '%s launching native commander life counter\n' \
  "$(date '+%Y-%m-%d %H:%M:%S')" >>"$LOG_FILE" 2>&1
"$BIN" "$STATE_FILE" >>"$LOG_FILE" 2>&1 &
child_pid=$!
wait "$child_pid"
status=$?
child_pid=
printf '%s native app exited with status %s\n' \
  "$(date '+%Y-%m-%d %H:%M:%S')" "$status" >>"$LOG_FILE" 2>&1
exit "$status"
