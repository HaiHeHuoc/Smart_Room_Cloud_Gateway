# Theme module

Private `app_gui` submodule for product-specific LVGL theme/style helpers.

- **Owner:** `app_gui`
- **Visibility:** private header; other components must use `app_gui` public
  models/commands instead of styling internals.
- **Lifecycle:** no independent LVGL owner; rendering still follows the
  `ui_manager_lvgl` synchronization contract.
- **Promotion rule:** keep private unless a reusable project-wide design system
  is intentionally introduced.
