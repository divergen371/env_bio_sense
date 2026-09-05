#pragma once

#include <cstddef>
#include <cstring>

namespace services {
namespace web_security {

constexpr size_t MAX_DATA_PATH = 120;

inline bool constantTimeEquals(const char* actual, const char* expected) {
    if (actual == nullptr || expected == nullptr) return false;
    const size_t actualLength = std::strlen(actual);
    const size_t expectedLength = std::strlen(expected);
    const size_t length = actualLength > expectedLength
        ? actualLength : expectedLength;
    unsigned difference = static_cast<unsigned>(actualLength ^ expectedLength);
    for (size_t i = 0; i < length; ++i) {
        const unsigned a = i < actualLength
            ? static_cast<unsigned char>(actual[i]) : 0u;
        const unsigned b = i < expectedLength
            ? static_cast<unsigned char>(expected[i]) : 0u;
        difference |= a ^ b;
    }
    return difference == 0;
}

inline bool endsWith(const char* value, const char* suffix) {
    if (value == nullptr || suffix == nullptr) return false;
    const size_t valueLength = std::strlen(value);
    const size_t suffixLength = std::strlen(suffix);
    return valueLength >= suffixLength &&
        std::memcmp(value + valueLength - suffixLength,
                    suffix, suffixLength) == 0;
}

inline bool normalizeDataPath(const char* input, char* output,
                              size_t outputCapacity) {
    if (input == nullptr || output == nullptr || outputCapacity < 3) {
        return false;
    }
    const size_t inputLength = std::strlen(input);
    const bool needsSlash = inputLength > 0 && input[0] != '/';
    const size_t normalizedLength = inputLength + (needsSlash ? 1u : 0u);
    if (inputLength == 0 || normalizedLength >= outputCapacity ||
        normalizedLength > MAX_DATA_PATH) {
        return false;
    }
    size_t write = 0;
    if (needsSlash) output[write++] = '/';
    char previous = '\0';
    for (size_t i = 0; i < inputLength; ++i) {
        const char c = input[i];
        const bool allowed = (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '_' || c == '-' || c == '.';
        if (!allowed || c == '\\' || (c == '/' && previous == '/')) {
            return false;
        }
        output[write++] = c;
        previous = c;
    }
    output[write] = '\0';
    if (output[0] != '/' || output[1] == '\0' ||
        std::strstr(output, "..") != nullptr ||
        std::strstr(output, "/./") != nullptr ||
        endsWith(output, "/.")) {
        return false;
    }
    return true;
}

inline bool isVisibleDataPath(const char* path) {
    if (path == nullptr || path[0] != '/' ||
        std::strstr(path, ".tmp") != nullptr) {
        return false;
    }
    const bool suffixAllowed = endsWith(path, ".csv") ||
        endsWith(path, ".zip") || endsWith(path, ".json") ||
        endsWith(path, ".ppg") || endsWith(path, ".incomplete");
    if (!suffixAllowed) return false;
    const char* nested = std::strchr(path + 1, '/');
    if (nested == nullptr) return true;
    return std::strncmp(path, "/data/ppg/", 10) == 0 ||
           std::strncmp(path, "/data/system/", 13) == 0;
}

inline bool normalizeVisibleDataPath(const char* input, char* output,
                                     size_t outputCapacity) {
    return normalizeDataPath(input, output, outputCapacity) &&
        isVisibleDataPath(output);
}

inline bool isPathInside(const char* path, const char* directory) {
    if (path == nullptr || directory == nullptr || directory[0] == '\0') {
        return false;
    }
    const size_t directoryLength = std::strlen(directory);
    return std::strncmp(path, directory, directoryLength) == 0 &&
        (path[directoryLength] == '\0' || path[directoryLength] == '/');
}

} // namespace web_security
} // namespace services
