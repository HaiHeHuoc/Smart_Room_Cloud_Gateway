# Telemetry JSON module

Private `cloud_manager` submodule for deterministic telemetry JSON formatting and
validation helpers.

- **Owner:** `cloud_manager`
- **Visibility:** private header/source; host tests may compile this module
  directly.
- **Policy boundary:** application telemetry schema remains owned by
  `cloud_manager`; this is not a generic JSON framework.
- **Promotion rule:** promote only if a provider-independent telemetry encoder
  becomes a real reusable requirement.
