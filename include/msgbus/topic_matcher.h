#pragma once

#include <string>
#include <string_view>

namespace msgbus {

/// MQTT-style topic wildcard matching.
///   '*' matches exactly one level   (e.g. "sensor/*/temp")
///   '#' matches zero or more levels (e.g. "sensor/#"), must be last segment
///
/// Returns true if the pattern matches the concrete topic.
inline bool topicMatches(std::string_view pattern, std::string_view topic) {
    size_t pi = 0, ti = 0;
    while (pi < pattern.size() && ti < topic.size()) {
        if (pattern[pi] == '#') {
            return true; // '#' matches everything remaining (including empty)
        }
        if (pattern[pi] == '*') {
            // '*' matches one level: skip to next '/' in both
            while (ti < topic.size() && topic[ti] != '/') ++ti;
            ++pi; // skip '*'
            // Both should now be at '/' or end
            if (pi < pattern.size() && pattern[pi] == '/') ++pi;
            if (ti < topic.size() && topic[ti] == '/') ++ti;
            continue;
        }
        if (pattern[pi] != topic[ti]) return false;
        ++pi;
        ++ti;
    }
    // Handle trailing '#' (matches zero levels)
    if (pi < pattern.size() && pattern[pi] == '#') return true;
    // Handle trailing '/#' when topic already ended (e.g. "sensor/#" vs "sensor")
    if (pi + 1 < pattern.size() && pattern[pi] == '/' && pattern[pi + 1] == '#')
        return true;
    return pi == pattern.size() && ti == topic.size();
}

/// Returns true if a pattern string contains wildcard characters.
inline bool isWildcard(std::string_view pattern) {
    return pattern.find_first_of("*#") != std::string_view::npos;
}

} // namespace msgbus
