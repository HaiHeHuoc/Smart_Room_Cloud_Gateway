# local_web_server

`local_web_server` is the bounded storage presentation edge for Sprint 19.
It starts only after `smart_room_app` receives the existing `ONLINE` network
handoff; it does not configure, reconnect, or otherwise control Wi-Fi.

Routes:

- `GET /` — compiled-in responsive Storage shell;
- `GET /api/storage/status` — SD availability and copied total/used/free bytes;
- `GET /api/storage/list?path=<logical-path>` — bounded direct-child metadata.

Additional routes are `GET /api/storage/download?path=<logical-path>`, raw-body
`POST /api/storage/upload?path=<logical-path>` (at most 8 MiB), and bounded
`POST` delete, rename, mkdir, and rmdir operations.

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
credentials, and any Sprint 20+ feature remain out of scope.

Host test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File components/application/local_web_server/test/host/run_tests.ps1
```
