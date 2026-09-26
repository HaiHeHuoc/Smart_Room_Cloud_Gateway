#include "local_web_icon_policy.h"

#include <string.h>

const char *local_web_icon_logical_path(const char *name)
{
    if (name == NULL) return NULL;
    if (strcmp(name, "storage") == 0) return "/web-icons/hard-drive.svg";
    if (strcmp(name, "folder") == 0) return "/web-icons/folder.svg";
    if (strcmp(name, "playback") == 0) return "/web-icons/music-2.svg";
    if (strcmp(name, "upload") == 0) return "/web-icons/upload.svg";
    if (strcmp(name, "volume") == 0) return "/web-icons/volume-2.svg";
    if (strcmp(name, "dashboard") == 0) return "/web-icons/layout-dashboard.svg";
    if (strcmp(name, "lights") == 0) return "/web-icons/lightbulb.svg";
    if (strcmp(name, "scenes") == 0) return "/web-icons/wand-sparkles.svg";
    if (strcmp(name, "logs") == 0) return "/web-icons/scroll-text.svg";
    if (strcmp(name, "diagnostics") == 0) return "/web-icons/activity.svg";
    if (strcmp(name, "sensor") == 0) return "/web-icons/thermometer.svg";
    if (strcmp(name, "network") == 0) return "/web-icons/wifi.svg";
    if (strcmp(name, "cloud") == 0) return "/web-icons/cloud.svg";
    if (strcmp(name, "time") == 0) return "/web-icons/clock-3.svg";
    if (strcmp(name, "up") == 0) return "/web-icons/arrow-up.svg";
    if (strcmp(name, "new_folder") == 0) return "/web-icons/folder-plus.svg";
    if (strcmp(name, "play") == 0) return "/web-icons/play.svg";
    if (strcmp(name, "pause") == 0) return "/web-icons/pause.svg";
    if (strcmp(name, "restart") == 0) return "/web-icons/rotate-ccw.svg";
    if (strcmp(name, "stop") == 0) return "/web-icons/square.svg";
    if (strcmp(name, "refresh") == 0) return "/web-icons/refresh-cw.svg";
    if (strcmp(name, "download") == 0) return "/web-icons/download.svg";
    return NULL;
}
