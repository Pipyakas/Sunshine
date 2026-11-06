# Pipyakas/Sunshine — personal fork

Goal: support per-session GPU selection for encode/capture by app, without giving up current Moonlight pairings on `47989-48010`.

## Branch layout
- `master`: mirrors `LizardByte/Sunshine: master` only. Do not commit here.
- `feat/per-app-encoder-adapter`: this feature (encode/capture render-device override per app).

## Feature: `encoder-adapter` in apps.json
- `apps.json` gains optional field `encoder-adapter` (string, VA-API/DRM render node, Linux only).
- Session-scoped render-device override is applied **before** display configuration and encoder probing at stream launch (see `src/nvhttp.cpp`), so the encoder selected for the session is the one on the app's GPU.
- Absent/empty `encoder-adapter` ⇒ behaves exactly as before (global `adapter_name`, else auto-detection).
- Non-existent path at launch ⇒ logs an error and falls back to global `adapter_name`.

## Files changed
- `src/platform/common.h` — `platf::set_render_device_override(const std::string &)` / `clear_render_device_override()`; `resolve_render_device()` honors override.
- `src/platform/linux/misc.cpp`, `src/platform/windows/misc.cpp`, `src/platform/macos/misc.mm` — implementations/no-op parity.
- `src/process.h|cpp` — `ctx_t::encoder_adapter`, `apps.json` parsing.
- `src/nvhttp.cpp` — set/clear override from the launched app's `encoder-adapter` before `configure_display`/`probe_encoders`.
- `docs/per_app_encoder_adapter.md` — docs.

## Caveats
- Cross-GPU streaming (capture GPU ≠ encode GPU) is unsupported; put both on the same GPU (e.g. iGPU for desktop, 6700 XT for gaming) via the app-level override.
- Override is per-session at launch time; resume does not re-probe.
