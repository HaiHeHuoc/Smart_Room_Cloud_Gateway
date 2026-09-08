# UI adapter module

Private `voice_assistant` submodule for converting voice state into copied UI
models and forwarding them through `app_gui`'s queue-based boundary.

- **Owner:** `voice_assistant`
- **Visibility:** private implementation; public adapter/model headers remain in
  `voice_assistant/include/` where composition needs them.
- **Concurrency rule:** callbacks never call LVGL directly.
- **Promotion rule:** promote only if the voice UI adapter becomes reusable
  independently from the parent orchestration lifecycle.
