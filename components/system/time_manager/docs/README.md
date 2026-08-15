# `time_manager` Component

Task 1 provides the independent time-service foundation: copied SNTP/timezone
configuration, ESP-NETIF SNTP lifecycle startup, thread-safe status snapshots,
and synchronized Unix/local/ISO-8601 time access.

Default configuration uses Google Public NTP (`time.google.com`) and Vietnam's
POSIX timezone (`ICT-7`). It owns no Wi-Fi, GUI, cloud, NVS, or application
task. SNTP completion updates the ESP-IDF system clock; callers read that clock
through `time_manager` only after `time_manager_is_synced()` is true.

Task 2 will add network-readiness notification and reconnection/resynchronization
orchestration. It will not be started by this component in Task 1.
