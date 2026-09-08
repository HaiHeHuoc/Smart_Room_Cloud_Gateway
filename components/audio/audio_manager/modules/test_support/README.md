# Test-support module

Private `audio_manager` submodule containing the optional public-API stress/test
coordinator implementation.

- **Owner:** `audio_manager`
- **Visibility:** implementation-private; the test entry point is exposed from
  `audio_manager/include/audio_api_test_task.h` when enabled by Kconfig.
- **Production behavior:** the test gate is default-off and does not create a
  task in normal production firmware.
- **Promotion rule:** keep test support with the component unless a repository-
  wide test harness later owns it explicitly.
