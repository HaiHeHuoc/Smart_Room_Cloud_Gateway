# Console module

Private `log_manager` submodule for the console-write wrapper used to keep
console coloring/filter behavior local to persistent logging integration.

- **Owner:** `log_manager`
- **Visibility:** private source only; producers use `app_log`, not this module.
- **Lifecycle:** no independent state machine or task.
- **Promotion rule:** keep private unless a second component needs the same
  console wrapper through an explicitly designed reusable API.
