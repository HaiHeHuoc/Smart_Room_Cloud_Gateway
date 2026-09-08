# WebSocket policy module

Private `xiaozhi_foundation` submodule for project-owned WebSocket task policy
and finite send-timeout wrappers around the managed provider transport.

- **Owner:** `xiaozhi_foundation`
- **Visibility:** private implementation; raw WebSocket handles do not escape the
  foundation boundary.
- **Policy:** wrappers protect project timeout/task constraints without modifying
  the managed `esp_xiaozhi` component.
- **Promotion rule:** keep private while it exists specifically to adapt the
  Xiaozhi provider transport.
