#include "services/logger.h"
#include <cstdio>
#include <cstdarg>

namespace services {

void Logger::init(uint32_t baudRate) {
    Serial.begin(baudRate);
}

void Logger::logArgs(LogLevel level, const char* module, const char* format, va_list args) {
    if (!Serial) return;

    const char* levelStr = "UNKN";
    switch (level) {
        case LogLevel::Debug:   levelStr = "DEBUG"; break;
        case LogLevel::Info:    levelStr = "INFO "; break;
        case LogLevel::Warning: levelStr = "WARN "; break;
        case LogLevel::Error:   levelStr = "ERROR"; break;
    }

    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);

    Serial.printf("[%06lu][%s][%s] %s\n", millis(), levelStr, module, buffer);
}

void Logger::log(LogLevel level, const char* module, const char* format, ...) {
    va_list args;
    va_start(args, format);
    logArgs(level, module, format, args);
    va_end(args);
}

} // namespace services
