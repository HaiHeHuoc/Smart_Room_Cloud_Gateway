# Common runtime services

`common` contains board-neutral, small project-runtime contracts. It owns no
task, driver, GPIO mapping, audio buffer, provider object, or application
lifecycle policy.

## Voice Recording Critical Window

`voice_recording_critical` is the bounded runtime signal named
`VOICE_RECORDING_CRITICAL` in design discussions. `voice_uplink` is its sole
writer. It enters only after `voice_assistant_audio_capture_start()` has
returned success; that bridge waits until the capture arbiter has observed
`AUDIO_MANAGER_STATE_RECORDING`, which occurs after audio_manager has enabled
I2S RX. The writer exits after the bounded capture-stop wait observes RX
inactive, or after the same terminal cleanup has been attempted on failure.

The state is keyed by session and PTT generation. A stale terminal path cannot
clear a newer live capture. Background components have only a copied query and
a small task-context transition-listener contract. Listener callbacks may only
notify their own worker; they must not block, log, allocate, or perform I/O.

This is cooperative workload prioritization, not a hard-real-time scheduler or
a permission to suspend arbitrary tasks. It does not alter Wi-Fi, TCP/IP, TLS,
Xiaozhi transport, I2S ownership, FreeRTOS priorities outside the documented
PTT adjustment, or CPU affinity.

The host regression at `test/host/run_tests.ps1` covers entry/exit ownership,
stale cleanup, listener edges, and more than fifty repeated turns. Target HIL
is still required for FreeRTOS timing and real I2S behavior.
