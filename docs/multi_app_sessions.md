# Multi-app concurrency in a single Sunshine instance (design draft)

Goal: one `sunshine` process, multiple Moonlight clients, multiple apps running at once
(e.g. Desktop on iGPU + Game Mode on 6700 XT), each in its own Moonlight window.

## Why stock Sunshine can't do this today
Stock has exactly one of each:
- `proc_t`: a single `_app_id`, `_process`, `_process_group` (src/process.h). `launch()` in
  `src/nvhttp.cpp` rejects a second app with "An app is already running on this host".
- Global capture/encode pipeline: `video::probe_encoders()` picks one `chosen_encoder`,
  one `capture_thread_ctx`, one render-device override (`platf::set_render_device_override`).
- Global display/output binding (`config::video.output_name`), one KWin/gamescope session
  at a time via `sunshine-mode-switch` (Plasma teardown).
- `currentgame`/`SUNSHINE_SERVER_FREE|BUSY` in serverinfo assumes one game per host.

The RTSP/session layer CAN hold multiple sessions (logs showed `active sessions: 2`),
but the app/capture/encode layers above it cannot.

## What a 1-instance fork must change (phased)
### Phase 1 — multiple processes in one proc manager
- `proc_t` holds `std::map<int, running_app_t>` (app_id → process/group/launch time).
- `launch()` rejects only a duplicate of the SAME app_id (resume), not a different app.
- `terminate()`/app-exit must stop only that app's group, not all peers.
- Tray/UI "currently playing" and `currentgame` become a list.

### Phase 2 — per-session capture/encoder (not global)
- Replace `chosen_encoder` + global `platf` render override with a per-session
  `encode_session_t` (encoder instance + render device + capture source).
- `encoder-adapter` (our existing fork field) must select a per-session render device,
  not a process-global override. VAAPI encode on iGPU + dGPU concurrently.
- `output_name`/display prep must be per app: Desktop events → KWin capture (iGPU),
  Game Mode → gamescope capture (6700 XT). Prep-cmds run per session concurrently.

### Phase 3 — concurrent compositors
- Desktop and Game Mode must not kill each other's session. Route:
  Plasma (KWin, iGPU card2) stays up; Game Mode runs as a nested/headless gamescope
  (child of the desktop session, 6700 XT card1) instead of the plasmalogin takeover
  (`gamescope-session-plus`) currently used by `sunshine-mode-switch`.
- Sunshine capture must grab the right Wayland output per session (KWin output vs
  gamescope pipewire node) simultaneously.

### Phase 4 — input/audio isolation
- Per-session virtual input mapping, audio sink routing, and focus/overlay handling.
- Moonlight needs no patch if the host advertises each app as launched independently;
  the client opens one window per launch. Verify with stock Moonlight before forking it.

## Non-goals / trade-offs
- Two concurrent sessions means two capture+encode pipelines: more GPU/CPU load,
  and the 610M iGPU will limit Desktop encode quality under load.
- This is a large fork. Upstream will not merge it as-is; it lives on
  `feat/multi-app-sessions` in Pipyakas/Sunshine.
