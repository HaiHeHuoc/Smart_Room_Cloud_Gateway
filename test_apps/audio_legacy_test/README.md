# Legacy Direct-I2S Audio Test Helper

This preserved helper predates `audio_manager` and directly allocates I2S/GPIO
resources. It is deliberately outside the production firmware and is not part
of the root ESP-IDF build. It has no `app_main()` entry point or automatic
Kconfig gate.

Use it only as source material for a separately scoped test application after
ensuring the production `audio_manager` is absent from that image. Do not add
it to the Gateway `main` CMake target or run it alongside the production audio
stack, because that would violate the single I2S owner contract.
