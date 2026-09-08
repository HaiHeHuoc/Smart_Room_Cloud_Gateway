# Buffer module

Private `log_manager` submodule implementing the bounded record ring/buffer used
by persistent logging.

- **Owner:** `log_manager`
- **Visibility:** private header/source; host tests may include it directly for
  deterministic buffer regression coverage.
- **Lifecycle:** allocation policy and writer lifecycle remain owned by
  `log_manager`.
- **Promotion rule:** promote only if the buffer obtains a standalone API and is
  reused outside logging.
