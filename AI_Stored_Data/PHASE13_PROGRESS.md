# Phase 13 Voice Assistant Progress

Updated: 2026-08-25
Branch: `phase/13-voice-assistant`
Current checkpoint: **13-E — FINAL CLOSURE**
Status: **SOFTWARE COMPLETE / BUILD PASS / HIL PASS**

## Collaboration result

Phase 13 was implemented in five reviewable checkpoints and is now at the final software closure point.

## 13-A — Foundation + State Machine — COMPLETE

Implemented `components/application/voice_assistant/` with:

- one long-lived orchestration task;
- bounded command queue and mutex waits;
- copied status callback outside the lock;
- non-zero session generation;
- stale generation rejection;
- states `UNINITIALIZED`, `INITIALIZED`, `IDLE`, `CONNECTING`, `READY`, `LISTENING`, `THINKING`, `SPEAKING`, `RECOVERING`, `ERROR`.

## 13-B — Xiaozhi Production Integration — COMPLETE

Added the long-lived project-owned Xiaozhi WebSocket session lifecycle inside `xiaozhi_foundation`:

```text
voice_assistant
-> xiaozhi_foundation public session API
-> public esp_xiaozhi lifecycle
-> real CONNECTED evidence
-> READY
```

The Phase-13 production session does not reuse the Phase-12 validation worker. Xiaozhi/MCP handles, endpoints, tokens, credentials, and callback-lifetime pointers remain private to the foundation.

## 13-C — Audio + GUI Contracts — COMPLETE

Added project-owned copied audio facts and the composition adapter:

- `voice_assistant_audio_status_t`;
- audio state inside `voice_assistant_status_t`;
- `voice_assistant_notify_audio_status()`;
- `voice_assistant_audio_adapter_post()`.

`audio_manager` remains the sole microphone/speaker/I2S/DMA/PCM owner. `voice_assistant` has no LVGL dependency and publishes only a copied UI-safe model.

Generic audio activity does not automatically imply LISTENING/THINKING/SPEAKING. Those states require an explicit voice-owned PTT/audio flow in Sprint 14.

## 13-D — Failure / Recovery + Robustness — COMPLETE

Implemented:

- intentional-stop vs unexpected-disconnect distinction;
- correct `active=false` semantics for failures while CONNECTING;
- late READY/ERROR filtering;
- generation filtering before state mutation;
- explicit `voice_assistant_recover()` with bounded one-shot cleanup;
- no automatic reconnect loop;
- one-public-command-at-a-time gating;
- latest-value audio-status coalescing so audio callback bursts cannot fill the lifecycle command queue.

HIL procedures and accepted evidence are recorded in:

`AI_Stored_Data/PHASE13_HIL_TEST_PLAN.md`

## 13-E — Final Closure — COMPLETE

### Production-vs-validation composition decision

`main.c` still contains the old Phase-12 validation composition under `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE`. That path is retained as test infrastructure and is **not deleted**.

For the Phase-13 production branch, `sdkconfig.defaults` now explicitly pins:

```text
CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=n
```

Therefore the normal Phase-13 image does not automatically register the temporary validation UI observer, route the Xiaozhi validation screen, or request the Phase-12 transport validator. Dedicated Phase-12/HIL branches may opt in explicitly.

This prevents the production branch from accidentally running the temporary validation lifecycle alongside the Phase-13 production session lifecycle.

### Why `main.c` does not auto-begin a production voice session

Phase 13 creates the adapter/orchestration foundation, but Sprint 14 owns the user-authorized Push-to-Talk transaction. Automatically opening a conversation at boot would violate the planned privacy/interaction model.

Therefore Phase 13 deliberately leaves `voice_assistant_begin_session()` trigger ownership to Sprint 14. The component is compiled into the application dependency graph, while the first user-authorized runtime composition/start trigger is a Sprint-14 integration task.

### Roadmap scope reconciliation

The older Xiaozhi roadmap wording placed all protocol event conversion inside Sprint 13. During implementation the project boundary was refined:

- transport readiness/failure/goodbye facts belong to Phase 13 and are already converted through project-owned session status;
- microphone uplink, Xiaozhi audio-channel commands, response audio and PTT/cancel belong to Sprint 14;
- transcript/emotion presentation and GUI queue/rendering belong to Sprint 15.

Do not pull Sprint-14/15 behavior backward merely to tick an outdated checkbox. Preserve the ownership boundaries established by the implemented architecture.

### Security boundary

Phase 13 exposes no voice API for:

- reboot;
- OTA;
- NVS erase/write;
- Wi-Fi reconfiguration;
- provisioning lifecycle;
- arbitrary GPIO/driver control;
- shell/system commands.

`voice_assistant` owns conversation orchestration only. Wi-Fi/provisioning/cloud/reset/storage/audio/LVGL remain owned by their existing project components.

## Final ownership

```text
wifi_manager / app_network_coordinator
    -> Wi-Fi + provisioning/network lifecycle

audio_manager
    -> sole microphone/speaker/I2S/DMA/PCM owner

xiaozhi_foundation
    -> sole direct esp_xiaozhi/MCP/service/session boundary

voice_assistant
    -> conversation generation, state, command ordering, recovery policy,
       copied audio/transport orchestration

app_gui / ui_manager_lvgl
    -> GUI queue/model and sole LVGL runtime ownership
```

## Software closure evidence

Confirmed by source/static review:

- bounded task/queue/mutex architecture;
- no direct I2S/LVGL/Wi-Fi/provisioning ownership leakage;
- no Xiaozhi/MCP handles or sensitive transport data in public voice APIs;
- stale generation filtering;
- bounded connection timeout and explicit recovery;
- duplicate lifecycle command rejection;
- audio callback burst coalescing;
- intentional-stop cleanup does not intentionally become transport failure;
- Phase-12 validation is default-OFF on the Phase-13 production branch;
- `main/CMakeLists.txt` already includes `voice_assistant` in the application dependency graph.

## Verification boundary

Target HIL confirmed on 2026-08-25:

- ESP-IDF 6.0.1 build/link;
- ESP32-S3 boot and steady Gateway runtime;
- real WebSocket start/stop across repeated generations;
- two 20-cycle stress runs plus a final 3-cycle regression;
- real AP loss, explicit recovery and bounded failed-connect recovery;
- duplicate gates, audio coalescing, generation isolation and resource checkpoints;
- automatic return to ONLINE after AP restoration without reset.

Mic/uplink/response-audio flow remains Phase-14 HIL scope.

## HIL acceptance

Accepted plan:

`AI_Stored_Data/PHASE13_HIL_TEST_PLAN.md`

The documented repeated lifecycle, failed-connect, transport-loss, intentional-stop, explicit-recovery, generation, queue-pressure and resource cases passed on target.

## Phase 13 final state

**PHASE 13 = SOFTWARE COMPLETE / BUILD PASS / HIL PASS (2026-08-25)**

Do not reopen Phase 13 for Sprint-14 PTT/audio features unless a build/static/HIL finding proves a Phase-13 bug. The next software development phase is Sprint 14 — Push-To-Talk Voice MVP.
