# Smart Room Target-Hardware Test Coordinators

`app_hil_test` holds default-off, target-only coordinators that used to live
beside the production composition root. It never owns I2S, DMA, GPIO/PTT, SD
file handles, or raw audio buffers; it invokes only public APIs after the
production audio lifecycle reaches READY.

## Gates

- `CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST` starts the existing public audio API
  stress task owned by `audio_manager`.
- `CONFIG_APP_PHASE16_AUTO_HIL_TEST` compiles and starts the bounded Phase-16
  arbitration HIL coordinator. It remains incompatible with the public audio
  stress task, exactly as before.

Both gates default to `n`. Normal production builds compile only the small
facade and perform no test work. Do not claim target acceptance from a build;
use the documented HIL matrix and serial evidence for a deliberately enabled
test profile.

To compile the Phase-16 profile without changing the normal local
`sdkconfig`, build in a separate directory with
`test_apps/sdkconfig.phase16_hil.defaults` appended to the project defaults:

```powershell
idf.py -B build-phase16-hil `
  -D SDKCONFIG="$PWD/build-phase16-hil/sdkconfig" `
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;test_apps/sdkconfig.phase16_hil.defaults" build
```
