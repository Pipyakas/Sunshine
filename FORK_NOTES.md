# Pipyakas/Sunshine — personal fork (D1 custom build)

Goal: support per-session GPU selection for encode/capture by app, without giving up
current Moonlight pairings on `47989-48010`.

## Branch layout
- `master`: mirrors `LizardByte/Sunshine: master` only. Do not commit here.
- `feat/per-app-encoder-adapter`: master-based experiment (superseded; do NOT deploy).
- `feat/per-app-encoder-adapter-v2026.914`: **canonical for D1**, based on upstream tag
  `v2026.914.233613` (`63d35f7`), matching the previously running custom build.

## Feature: `encoder-adapter` in apps.json
- `apps.json` gains optional field `encoder-adapter` (string, VA-API/DRM render node, Linux only).
- Session-scoped render-device override is applied **before** display configuration and encoder
  probing at stream launch (see `src/nvhttp.cpp`), so the encoder selected for the session is
  the one on the app's GPU.
- Absent/empty `encoder-adapter` ⇒ behaves exactly as before (global `adapter_name`, else
  auto-detection).
- Non-existent path at launch ⇒ logs an error and falls back to global `adapter_name`.
- Every launch with an override logs: `Using per-app encoder-adapter [...] for this streaming session`.

Live D1 wiring (`~/.config/sunshine/apps.json`):
- `Desktop` → `encoder-adapter: /dev/dri/renderD129` (Raphael iGPU: KWin capture + VAAPI encode)
- `Game Mode` → `encoder-adapter: /dev/dri/renderD128` (RX 6700 XT: gamescope capture + VAAPI encode)

## Fix: `capture = dynamic`
- Stock upstream only accepts literal backend names (`kwin`, `wlr`, `kms`, `portal`, `x11`,
  `nvfbc`); `capture = dynamic` matches none and aborts capture init.
- This fork maps `dynamic` to `kwin` on `XDG_CURRENT_DESKTOP=KDE` sessions and to auto-select
  elsewhere, matching the previous D1 custom build behaviour
  ("Dynamically capturing KDE through KWin").

## Build (D1 / Bazzite 45, linuxbrew)
- CUDA off (`-DSUNSHINE_ENABLE_CUDA=OFF`), docs off (`-DBUILD_DOCS=OFF`, required on this tag:
  Doxygen is mandatory otherwise), tests off (upstream tests clobber global CXX_FLAGS).
- Install prefix `~/.local/opt/sunshine-wayland`, `SUNSHINE_ASSETS_DIR=assets`, so the binary
  looks for shaders/web assets in `<prefix>/assets` (as the previous custom build did).
- RPATH must include `/home/linuxbrew/.linuxbrew/lib`,
  `.../opt/qtbase/lib`, `.../opt/qtsvg/lib`, `.../opt/libdrm/lib` — brew Qt 6.12 lives in
  `opt/qtbase`, outside the default `lib/` runpath.
- After copying the binary into place: `sudo setcap cap_sys_admin,cap_sys_nice+p <bin>`.

## Files changed (vs v2026.914.233613)
- `src/platform/common.h` — `platf::set_render_device_override()` / `clear_render_device_override()`.
- `src/platform/linux/misc.cpp`, `src/platform/windows/misc.cpp`, `src/platform/macos/misc.mm` — impl/no-op parity + `capture=dynamic` mapping.
- `src/process.h|cpp` — `ctx_t::encoder_adapter`, `apps.json` parsing.
- `src/nvhttp.cpp` — apply/clear override from the launched app before `configure_display()`/`probe_encoders()`.
- `docs/per_app_encoder_adapter.md` — docs.

## Caveats
- Cross-GPU streaming (capture GPU ≠ encode GPU) is unsupported; put both on the same GPU
  (iGPU for desktop, 6700 XT for gaming) via the app-level override.
- Override is per-session at launch time; resume does not re-probe.
