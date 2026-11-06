# Per-app encoder adapter (`encoder-adapter`)

Sunshine normally selects a single GPU for encoding for the whole process, via the global
`adapter_name` setting (or display-aware auto-detection when unset). Some hosts want
different GPU workloads for different apps (for example: desktop streaming on an iGPU
and game streaming on a discrete GPU), and one Sunshine process cannot change its global
adapter between Moonlight sessions.

Apps can override the render device used for encode and hardware capture for their own
sessions by setting the app-level `encoder-adapter` field in `apps.json`:

```json
{
  "name": "Game Mode",
  "cmd": "",
  "encoder-adapter": "/dev/dri/renderD128",
  "prep-cmd": [
    { "do": "sh -c '/home/quynn/.local/bin/sunshine-app-mode game'", "undo": "sh -c '/home/quynn/.local/bin/sunshine-virtual-monitor stop'" }
  ]
}
```

Notes and rules:

- `encoder-adapter` is the VA-API/DRM render node (for example `/dev/dri/renderD128`). On Linux it
  selects the same GPU Sunshine uses for capture and for VA-API encode during that app's
  streaming session. It is ignored on other platforms.
- Empty or absent means: unchanged behaviour (global `adapter_name`, or auto-detection).
- The override is session-scoped: it applies when the app's stream starts, before display
  configuration and encoder probing run. After the stream ends, the override persists
  harmlessly in memory; the next app launch overwrites it either with its own value or
  with the empty default.
- If the configured path does not exist at launch time, Sunshine logs an error and falls
  back to the global `adapter_name` setting instead of aborting the session.
- Cross-GPU capture: when the app's GPU cannot share memory with the capture path, the
  usual GPU -> RAM -> GPU bridge is used automatically.

Apply the tuned adapter only for apps that need it. Leave `encoder-adapter` unset on the
rest so global preferences carry across those sessions.
