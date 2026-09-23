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
    return NULL;
}
