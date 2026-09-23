# local_web_server

`local_web_server` is the bounded storage presentation edge for Sprint 19.
It starts only after `smart_room_app` receives the existing `ONLINE` network
handoff; it does not configure, reconnect, or otherwise control Wi-Fi.

Routes:

- `GET /` — compiled-in responsive Storage shell;
- `GET /api/storage/status` — SD availability and copied total/used/free bytes;
- `GET /api/storage/list?path=<logical-path>` — bounded direct-child metadata.

Additional routes are `GET /api/storage/download?path=<logical-path>` (with a
browser download filename through `Content-Disposition`), raw-body
`POST /api/storage/upload?path=<logical-path>` (at most 8 MiB), and bounded
`POST` delete, rename, mkdir, and rmdir operations.

The HTTP server reserves 16 URI-handler slots, exactly matching the routes
registered by this component. A route addition must update that bounded count;
otherwise ESP-IDF returns `ESP_ERR_HTTPD_HANDLERS_FULL` and startup rolls back.

The component calls only public `sd_card_manager` APIs. The SD manager keeps
mount/recovery and VFS lease ownership and exposes only copied metadata. The
logical Web root `/` is mapped internally to the approved mounted SD root; raw
VFS paths and filesystem handles never enter HTTP responses.

`local_web_path_policy_normalize()` decodes one URL-encoded path and rejects
traversal, malformed encoding, empty/dot or trailing components, duplicate
separators, control characters, backslashes, and overlong input. The SD manager
also rejects its reserved upload-temporary suffix. The current bounds are a
192-byte logical path, a 64-byte filename, 32 returned entries, and 64 scanned
entries. Directory reads are non-recursive.

`sd_card_manager` permits one Web transfer or mutation at a time. Downloads use
one 4 KiB chunk buffer; uploads write a hidden `.webupload-partial` sibling,
then publish only after close and rename. A failed or disconnected upload is
aborted and its managed SD lease is released. HTTP handlers never retain raw
VFS or `FILE` handles.

No WebSocket is used in V1: request/response plus XHR provides exact browser
upload progress without another persistent connection. `app_gui` owns the LCD
`WEB_STORAGE` surface; this component posts only copied status updates and
never calls LVGL. Audio, lights, dashboard/system control, Wi-Fi/provisioning,
credentials remain out of scope.

## Playback presentation edge

Sprint 20 adds a local playback tab without giving HTTP ownership of audio
resources. `GET /api/audio/status` exposes only copied control state, source
kind, resumability, generation, committed/total frames, and the owner-selected
`sample_rate_hz`; the browser derives human-readable time from that rate and
does not assume a fixed sample rate. `POST /api/audio/seek?frames=<n>&generation=<g>`
requires one decimal frame target and the current generation for the local
source. It rejects an
invalid/out-of-range target, stale generation, paused-by-PTT source, or a
non-seekable live PCM/Xiaozhi source. The browser uses the owner-published
committed-frame granularity as the range step, so a committed request cannot
start a WAV reader at an invalid byte offset. It sends one request when a
range drag is committed, while polling never overwrites an in-progress drag.

The handler delegates solely to the public `voice_assistant` playback facade;
that facade delegates to `audio_manager`, which remains the I2S, WAV reader,
and SD-lease owner. No Web handler opens a WAV, seeks a `FILE *`, or changes
I2S configuration. Playback speed control is intentionally deferred: a
rate-change feature would require an explicit DSP/resampling design rather
than changing the global I2S rate from the UI.

## SD-backed presentation icons

The Web chrome loads five fixed SVG assets through
`GET /api/assets/icon?name=<storage|folder|playback|upload|volume>`. The
endpoint maps those tokens to fixed `/web-icons/*.svg` paths and streams them
through the existing `sd_card_manager_download_*` lease-owned API with
`image/svg+xml`; arbitrary paths, raw VFS paths, and non-allowlisted files are
not served. The expected SD-card directory is `/web-icons/`, with the Lucide
ISC license retained as `/web-icons/LICENSE-LUCIDE.txt`.

Host test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File components/application/local_web_server/test/host/run_tests.ps1
```
