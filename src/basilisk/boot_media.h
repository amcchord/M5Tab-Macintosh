// Startup Settings and automatic boot share the selected disk and media checks.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline int BootSelectDisk(const std::vector<std::string> &files, char *selected, size_t capacity)
{
    if (!selected[0] && !files.empty())
        snprintf(selected, capacity, "%s", files.front().c_str());
    const auto it = std::find(files.begin(), files.end(), selected);
    return it == files.end() ? -1 : static_cast<int>(it - files.begin());
}

inline bool BootHasMedia(bool from_cd, const char *disk, const char *cdrom,
                         bool (*usable)(const char *, size_t, size_t))
{
    const char *media = from_cd ? cdrom : disk;
    return media[0] && usable("/Q650.ROM", 64 * 1024, 1024 * 1024) && usable(media, 512, SIZE_MAX);
}
