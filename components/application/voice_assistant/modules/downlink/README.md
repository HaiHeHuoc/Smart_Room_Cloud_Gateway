# Downlink module

Private `voice_assistant` submodule for converting bounded Xiaozhi downlink audio
into the `audio_manager` streaming/playback boundary.

- **Owner:** `voice_assistant`
- **Visibility:** implementation-private; public downlink contracts remain in
  `voice_assistant/include/`.
- **Ownership rule:** it never bypasses `audio_manager` arbitration or takes I2S
  ownership directly.
- **Promotion rule:** keep private unless downlink transport becomes a stable
  reusable service independent from voice orchestration.

Each active server response has a project-owned nonzero response epoch in
addition to the long-lived WebSocket client generation. Queue items, timeout,
terminal, and GPIO38 interruption paths must match both values before they can
write PCM or finish the corresponding playback turn. Callback admission is
disabled before terminal cleanup, and stale queued items are discarded rather
than reset wholesale. A queue-full GPIO38 interrupt also leaves a separate
pending terminal intent for this task to consume, so it cannot be lost merely
because the callback queue is full.

The uplink owner reserves that epoch before it transmits MANUAL
`stop-listening`. Foundation admits only this reserved response immediately
before the synchronous transmit, so the first TTS/Opus callback cannot be
dropped in the send-return race. Cleanup-only stop paths leave delivery closed.

The provider exposes no server response identifier, so the private epoch alone
only protects local queue/staging and cancellation races. A local abort now
also records a required transport fence and sends the protocol abort before
the channel's session ID is closed. Before the next PTT capture on that client
generation, `voice_assistant` reserves a replacement generation, stops the old
WebSocket task, drains its already-posted global events through a private FIFO
marker, and starts a fresh transport while retaining the project MCP engine.
PTT stays `ARMING_SESSION` until that replacement connection is READY. Thus
raw packets from the old TCP/TLS/WebSocket connection cannot be admitted to the
next response; an unsuccessful fence fails closed rather than authorizing
capture.
