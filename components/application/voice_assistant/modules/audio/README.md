# Audio adapter module

Private `voice_assistant` submodule that translates copied `audio_manager` state
and arbitration events into voice-orchestration commands.

- **Owner:** `voice_assistant`
- **Visibility:** implementation-private; public bridge contracts remain under
  `voice_assistant/include/` where needed by application composition.
- **Ownership rule:** this module never owns I2S, DMA, or audio buffers; those
  remain owned by `audio_manager`.
- **Promotion rule:** keep private unless the adapter becomes independently
  reusable by another orchestration component.
