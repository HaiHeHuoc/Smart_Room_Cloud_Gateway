# local_web_server

`local_web_server` is the bounded storage presentation edge for Sprint 19.
It starts only after `smart_room_app` receives the existing `ONLINE` network
handoff; it does not configure, reconnect, or otherwise control Wi-Fi.

Routes:

- `GET /` — compiled-in responsive Dashboard, Storage, Playback, and Lights shell;
- `GET /api/storage/status` — SD availability and copied total/used/free bytes;
- `GET /api/storage/list?path=<logical-path>` — bounded direct-child metadata.

Additional routes are `GET /api/storage/download?path=<logical-path>` (with a
browser download filename through `Content-Disposition`), raw-body
`POST /api/storage/upload?path=<logical-path>` (at most 8 MiB), and bounded
`POST` delete, rename, mkdir, and rmdir operations.

The HTTP server reserves 20 URI-handler slots for the current 19 registered
routes, retaining one bounded spare slot. A route addition must still review
that count; otherwise ESP-IDF returns `ESP_ERR_HTTPD_HANDLERS_FULL` and startup
rolls back.

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
`WEB_STORAGE` and `WEB_LIGHT` surfaces; this component posts only copied
status updates and never calls LVGL. Audio, dashboard/system control,
Wi-Fi/provisioning, and credentials remain out of scope.

While the browser document is visible, the Storage presentation polls its
copied status endpoint every two seconds. A transition to unavailable clears
the stale listing and capacity facts, disables Storage mutation controls, and
shows an SD-unavailable state without a page reload. A later transition to
ready reloads the current directory and audio catalog once. This remains
browser polling only: it does not add a WebSocket or give the Web component SD
mount/recovery ownership.

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

## Dashboard status backend

Sprint 22.1 adds the read-only `GET /api/dashboard/status` route. It streams
a bounded JSON snapshot with `Cache-Control: no-store`; there is no Dashboard
write route and no browser-owned status cache, task, or driver access.

The route queries only copied public manager snapshots for sensor quality,
SD lifecycle/capacity/leases, audio lifecycle and playback summary, product
light state, cloud lifecycle, Wi-Fi connection/IP/RSSI facts, time sync, and
monotonic uptime. A manager snapshot failure degrades only its section to
`available:false`; the endpoint remains HTTP 200 whenever the bounded response
can be constructed. `overall_state` is `ready` only when every section is
normal, `attention` when any section needs attention or is unavailable while
another remains available, and `unavailable` only when every section snapshot
is unavailable.

Sensor readings are numeric only when the copied value is valid, current, and
finite. Stale, failed, or non-finite readings are emitted as JSON `null`, never
as a failed-read sentinel. SD capacity is queried only while the lifecycle is
READY; a capacity lookup failure emits `capacity_valid:false` and `null`
capacity fields rather than fabricated zero READY storage.

The dashboard deliberately omits SSID, passwords, provisioning/NVS data,
Firebase/authentication material, filesystem paths, audio payloads, I2S/DMA
state, and performance-monitor metrics. `performance_monitor` currently logs
metrics but has no copied public snapshot, so CPU/heap/resource fields remain
deferred.

## Dashboard presentation edge

Sprint 22.2 makes Dashboard the first, default tab over the existing read-only
status route. It displays only copied operational facts: overall state, sensor
validity/freshness, SD capacity and leases, Wi-Fi/IP/RSSI, audio
playback/source/volume, logical light state, cloud state, time sync, and
monotonic uptime. There are no Dashboard controls, Wi-Fi actions,
configuration fields, secrets, paths, driver details, or performance-monitor
log parsing.

Dashboard uses a CSS Grid with one card at narrow mobile widths, two cards at
tablet widths, and three cards on larger screens. It preserves the native
accessible tablist: each tab has `role=tab`, `aria-controls`, selected state,
and roving `tabindex`; Left/Right arrows and Home/End move and activate the
corresponding tab.

While the document is visible and Dashboard is active, exactly one status
request may be in flight and the next request is scheduled two seconds after
the previous request completes. Leaving the tab or hiding the document stops
future Dashboard scheduling; returning immediately refreshes only the active
tab. A per-request generation discards a response made stale by a tab change.
If a request fails, the last accepted card values remain visible and the UI
reports the failure plus the age of that snapshot. Rendering writes all
server-derived values through DOM `textContent`, never `innerHTML`.

Sprint 22.3 hardening treats a sensor timestamp later than the monotonic clock
as an unknown sample age rather than a false “just now” value. The browser
accepts Dashboard numeric fields only when they are finite JSON numbers and
checks storage capacity bounds before calculating a percentage. If the user
leaves and re-enters Dashboard while its final request is still in flight, one
queued refresh runs after that request completes; this preserves the
one-in-flight limit without leaving the tab unscheduled.

## Light REST edge

`GET /api/light/status` returns a copied `light_manager` product state. An
uninitialized manager returns `{ "ok": true, "available": false }`; a lock
timeout or lower-layer failure returns a deterministic `503 light_busy` or
`500 light_status_failed` error.

`POST /api/light/state` accepts a bounded query (under 160 bytes) with one or
more optional exact fields: `power=true|false`, `red=0..255`, `green=0..255`,
`blue=0..255`, `brightness=0..100`, and one of the thirteen effect tokens
`solid|blink|breath|pulse|rainbow|strobe|heartbeat|candle|sos|lightning|wake_up|sleep_fade|notification`. Unknown, duplicate, malformed, or empty
requests are rejected before reading or changing product state. The handler
copies the current state, applies only provided fields, calls exactly one
`light_manager_set_state()`, then returns a read-back snapshot.

An effect request follows existing MCP semantics: it activates power when
power is omitted, and a black state with no supplied RGB gets neutral white so
the effect is visible. Combining an effect with `power=false` is rejected.
OFF otherwise preserves RGB, brightness, and the selected effect; brightness
zero remains a valid logical ON state. The Web component never includes or
calls NeoPixel, LED-strip, RMT, GPIO, or effect-worker APIs.

## Light presentation edge

The third accessible tab, `Lights`, retains the dark compact layout and uses
native color, range, checkbox, and select controls for logical power, RGB,
brightness, and effect. Color and brightness input is coalesced for 250 ms;
power and effect commit immediately. Every successful commit then reads the
authoritative status. A local generation prevents an older poll or response
from overwriting a newer edit, and only one Light poll is active. The
two-second Light poll runs only while the visible document has the Lights tab
open. Unavailable status clears and disables controls without a browser alert.
The transient `light_busy` response instead preserves the last copied controls
and reports that state will refresh, so a lock timeout is not misrepresented as
manager deinitialization. Lower-layer apply failures retain their rejection
feedback while a read-only reconciliation occurs.
OFF preserves the server-owned RGB/brightness/effect values; Rainbow displays
the retained logical RGB while noting that physical output is dynamic. The
single-LED additions are Strobe (fast flash), Heartbeat (double pulse), Candle
(continuous brightness flicker), SOS, Lightning, Wake Up, Sleep Fade, and
Notification. Notification is a repeating double flash because the existing
single product state has no separate restore-state owner. Wake Up ends stable
at the selected brightness; Sleep Fade latches dark until a later command.
Strip-only chase/wipe patterns remain intentionally unavailable.

`local_web_server` maps a read-back `light_manager_state_t` into a copied
`ui_web_light_status_t` queue payload. `app_gui` renders that payload on the
`WEB_LIGHT` LCD view; the HTTP task never accesses LVGL and the LCD is not an
interactive controller.

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
