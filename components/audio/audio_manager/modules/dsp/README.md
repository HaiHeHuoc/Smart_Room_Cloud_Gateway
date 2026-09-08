# DSP module

Private `audio_manager` submodule for microphone sample conversion, audio DSP,
and cooperative processing helpers.

- **Owner:** `audio_manager`
- **Visibility:** private headers and sources; they are not a public DSP library
  contract for other components.
- **Lifecycle:** no independent task/service ownership beyond work invoked by
  the parent audio manager.
- **Promotion rule:** promote only after a separate, stable DSP API is required
  by more than one parent component.
