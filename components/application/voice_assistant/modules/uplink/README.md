# Uplink module

Private `voice_assistant` submodule for moving bounded microphone/audio frames
from the audio boundary into the Xiaozhi uplink path.

- **Owner:** `voice_assistant`
- **Visibility:** implementation-private; callers use the public voice/uplink
  contracts in `voice_assistant/include/`.
- **Ownership rule:** it does not own microphone I2S or audio-manager buffers.
- **Promotion rule:** keep private unless uplink transport becomes a separately
  reusable service.

After a non-empty PTT turn, this module reserves the bounded downlink response
epoch before it sends MANUAL `stop-listening`. It then uses the dedicated
foundation response-stop call, which admits that already-reserved response just
before transmit; no cleanup/cancel path opens the delivery gate.
