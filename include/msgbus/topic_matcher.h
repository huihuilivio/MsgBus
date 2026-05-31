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
            while (ti < topic.size() && topic[ti] != '/')
                ++ti;
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
    if (pi + 1 < pattern.size() && pattern[pi] == '/' && pattern[pi + 1] == '#') return true;
    return pi == pattern.size() && ti == topic.size();
}

/// Returns true if a pattern string contains wildcard characters.
inline bool isWildcard(std::string_view pattern) {
    return pattern.find_first_of("*#") != std::string_view::npos;
}

/// Compile-time MQTT topic validation.
/// Returns true if the topic/pattern is valid according to MQTT rules.
constexpr bool isValidTopic(std::string_view topic) {
    if (topic.empty()) return false;

    bool in_segment = false;
    bool last_was_wildcard = false;

    for (size_t i = 0; i < topic.size(); ++i) {
        char c = topic[i];

        if (c == '/') {
            if (last_was_wildcard) return false;     // wildcard must be entire segment
            if (i == topic.size() - 1) return false; // trailing slash not allowed
            in_segment = false;
            last_was_wildcard = false;
        } else if (c == '#') {
            if (!in_segment && (i == 0 || topic[i - 1] == '/')) {
                return i == topic.size() - 1; // '#' must be last character
            }
            return false;
        } else if (c == '*') {
            if (!in_segment && (i == 0 || topic[i - 1] == '/')) {
                last_was_wildcard = true;
                in_segment = true;
            } else {
                return false; // '*' must be entire segment
            }
        } else {
            if (last_was_wildcard) return false; // wildcard must be entire segment
            in_segment = true;
        }
    }

    return true;
}

/// Helper for compile-time topic validation with static_assert.
template <size_t N> struct TopicValidator {
    static constexpr bool validate(const char (&topic)[N]) {
        return isValidTopic(std::string_view(topic, N - 1));
    }
};

/// Compile-time topic tag for use in subscribe().
/// Use like: bus.subscribe<int>(topic_tag("sensor/temp"), handler);
template <size_t N> struct topic_tag {
    static constexpr size_t size = N;
    const char (&str)[N];

    constexpr explicit topic_tag(const char (&s)[N]) : str(s) {
        static_assert(TopicValidator<N>::validate(s),
                      "Invalid MQTT topic: must follow MQTT topic rules");
    }

    constexpr explicit operator std::string_view() const { return std::string_view(str, N - 1); }
};
} // namespace msgbus

/// Compile-time topic validation macro.
/// Usage: MSGBUS_VALIDATE_TOPIC("sensor/temp")
#define MSGBUS_VALIDATE_TOPIC(topic)                                                               \
    static_assert(msgbus::TopicValidator<sizeof(topic)>::validate(topic),                          \
                  "Invalid MQTT topic: " #topic)
