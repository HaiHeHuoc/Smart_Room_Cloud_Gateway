# Allocator module

Private `ui_manager_lvgl` submodule for the LVGL PSRAM allocation bridge used by
the configured custom allocator path.

- **Owner:** `ui_manager_lvgl`
- **Visibility:** private source; application/UI components do not allocate
  through this module directly.
- **Lifecycle:** LVGL initialization and synchronization remain owned by the
  parent UI manager.
- **Promotion rule:** keep private unless allocator policy becomes a separately
  reusable platform component.
