# Push-to-talk module

Private `voice_assistant` submodule for push-to-talk state handling and the
current GPIO-backed PTT adapter.

- **Owner:** `voice_assistant`
- **Visibility:** implementation-private; public PTT contracts remain under
  `voice_assistant/include/`.
- **Hardware boundary:** GPIO acquisition is an input adapter only; voice session
  policy remains in the parent component.
- **Promotion rule:** promote only if the input adapter is reused independently
  from voice orchestration.
