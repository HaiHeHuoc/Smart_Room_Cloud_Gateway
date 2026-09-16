# local_web_server

`local_web_server` is the read-only presentation edge for Sprint 19 Prompt 1.
It starts only after `smart_room_app` receives the existing `ONLINE` network
handoff; it does not configure, reconnect, or otherwise control Wi-Fi.

Routes:

- `GET /` — compiled-in responsive Storage shell;
- `GET /api/storage/status` — SD availability and copied total/used/free bytes;
- `GET /api/storage/list?path=<logical-path>` — bounded direct-child metadata.

The component calls only public `sd_card_manager` APIs. The SD manager keeps
mount/recovery and VFS lease ownership and exposes only copied metadata. The
logical Web root `/` is mapped internally to the approved mounted SD root; raw
VFS paths and filesystem handles never enter HTTP responses.

`local_web_path_policy_normalize()` decodes one URL-encoded path and rejects
traversal, malformed encoding, empty/dot components, duplicate separators,
control characters, backslashes, and overlong input. The current bounds are a
192-byte logical path, a 64-byte filename, 32 returned entries, and 64 scanned
entries. Directory reads are non-recursive.

This checkpoint has no upload, download, delete, rename, mkdir/rmdir, progress,
WebSocket, Web-remote LCD, audio, lights, dashboard, or system-control route.

Host test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File components/application/local_web_server/test/host/run_tests.ps1
```
