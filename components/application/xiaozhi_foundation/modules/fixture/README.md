# Validation fixture module

Private, validation-only `xiaozhi_foundation` submodule for Phase-2F fixture
loading/buffering used by transport validation modes.

- **Owner:** `xiaozhi_foundation`
- **Visibility:** private test implementation; it is not a production data path.
- **Runtime gate:** fixture sources are included only by the existing validation
  Kconfig selection.
- **Storage rule:** the SD fixture path uses `sd_card_manager`; production
  Xiaozhi lifecycle must not depend on fixture availability.
- **Promotion rule:** move to a dedicated test component only if repository-wide
  validation ownership is redesigned explicitly.
