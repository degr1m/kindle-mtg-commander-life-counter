# Changelog

All notable changes to the MTG Commander Life Counter are documented here.

## [2.0.0] - 2026-07-14

### Added

- Four `P1`–`P4` commander-damage counters for every player.
- Atomic commander-damage tracking: damage increments reduce life, while hold-to-clear refunds the tracked amount.
- Per-player poison counters with tap-to-increment and hold-to-clear behavior.
- Updated Monarch control with tap-to-assign/transfer and hold-to-clear behavior.
- Directional First-player markers and a `Pick First` randomizer.
- Confirmed New Game reset for all V2 state.
- In-app Menu with live battery status, front-light control, Back to Game, and saving Exit.

### Kindle integration

- Exact front-light brightness restoration after physical sleep/wake when the light was previously on.
- Light-off preservation when sleeping with the front light already off.
- Fail-closed touch gating while asleep or while physical power state is unresolved.
- Front-light readback confirmation for sleep, wake, and Exit transitions.
- Bounded LIPC property reads, event parsing, listener restart, and reconciliation watchdogs.
- Normal Exit confirms the front light is off before closing.

### Delivery and compatibility

- Native GTK+-2.0/Cairo/Pango application compiled as a stripped ARM hard-float `kindlehf` executable.
- MTP installer preserves the existing `data/` folder and verifies every uploaded file by readback.
- Reproducible release ZIP generation through `scripts/package-release.sh`.
- V2 on-device screenshot from an Amazon Kindle Paperwhite.

### Upgrade notes

- V1 save files migrate to V2 with new counters initialized safely.
- The application remains local, offline, and free of paid or cloud dependencies.
- The release ZIP installs under `extensions/mtg-life-counter/` and intentionally excludes runtime state and logs.
