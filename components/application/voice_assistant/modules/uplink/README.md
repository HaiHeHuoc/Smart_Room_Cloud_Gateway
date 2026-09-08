# Uplink module

Private `voice_assistant` submodule for moving bounded microphone/audio frames
from the audio boundary into the Xiaozhi uplink path.

- **Owner:** `voice_assistant`
- **Visibility:** implementation-private; callers use the public voice/uplink
  contracts in `voice_assistant/include/`.
- **Ownership rule:** it does not own microphone I2S or audio-manager buffers.
- **Promotion rule:** keep private unless uplink transport becomes a separately
  reusable service.
