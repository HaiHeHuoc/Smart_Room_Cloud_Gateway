#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and compose Smart Room product services.
 *
 * Called once by the ESP-IDF `app_main()` entrypoint. This product component
 * owns startup ordering, copied inter-component callback routing, deferred
 * cloud/audio startup policy, and boot-failure presentation. Manager and
 * driver ownership remains in their respective components.
 */
void smart_room_app_start(void);

#ifdef __cplusplus
}
#endif
