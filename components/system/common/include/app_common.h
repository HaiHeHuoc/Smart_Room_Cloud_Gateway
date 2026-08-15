#pragma once

#include "sdkconfig.h"

/** Human-readable firmware identity used in logs and status screens. */
#define APP_PROJECT_NAME "ESP32-S3 Smart Room Cloud Gateway"

/** Manually maintained semantic firmware version. */
#define APP_PROJECT_VER "1.0.0"

/** Release date string; this value is not generated from the build time. */
#define APP_PROJECT_VER_DATE "2026-08-02"

/*
 * Development Firebase configuration comes from menuconfig and the generated
 * local sdkconfig.h. The repository intentionally contains no real account
 * credentials. These values are compiled into the development firmware and
 * are not a production secret-storage mechanism.
 */
#define FIREBASE_API_KEY             "AIzaSyBXsyDzNYGd0xxRDvms8nnwtuIYwR3h8ks"
#define FIREBASE_DEVICE_EMAIL        "tranlonghai21@gmail.com"
#define FIREBASE_DEVICE_PASSWORD     "Musaking888"
#define FIREBASE_DEVICE_UID          "0leuYu7fCMRnM8w5bvv8VzquPgV2"
