# Codec module

Private `voice_assistant` submodule for Opus codec helpers used by the voice
uplink/downlink path.

- **Owner:** `voice_assistant`
- **Visibility:** private implementation header/source; provider or codec handles
  are not exposed as application ownership.
- **Lifecycle:** codec work is driven by the parent voice session lifecycle.
- **Promotion rule:** promote only if a provider-independent codec service is
  required by multiple components.
