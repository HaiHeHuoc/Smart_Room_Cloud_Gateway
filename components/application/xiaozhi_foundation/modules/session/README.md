# Session module

Private `xiaozhi_foundation` submodule for provider session lifecycle bridging and
copied project-owned session state.

- **Owner:** `xiaozhi_foundation`
- **Visibility:** private source; provider handles never cross the public
  foundation API.
- **Lifecycle:** service/session ownership remains coordinated through the
  parent foundation boundary.
- **Promotion rule:** keep private unless session handling becomes a separate
  provider-neutral component.
