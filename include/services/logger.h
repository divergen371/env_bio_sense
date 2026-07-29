#pragma once

#include <Arduino.h>

namespace services {

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static void init(uint32_t baudRate = 115200);
    static void log(LogLevel level, const char* module, const char* format, ...);

    static void debug(const char* module, const char* format, ...) {
        va_list args;
        va_start(args, format);
        logArgs(LogLevel::Debug, module, format, args);
        va_end(args);
    }

    static void info(const char* module, const char* format, ...) {
        va_list args;
        va_start(args, format);
        logArgs(LogLevel::Info, module, format, args);
        va_end(args);
    }

    static void warn(const char* module, const char* format, ...) {
        va_list args;
        va_start(args, format);
        logArgs(LogLevel::Warning, module, format, args);
        va_end(args);
    }

    static void error(const char* module, const char* format, ...) {
        va_list args;
        va_start(args, format);
        logArgs(LogLevel::Error, module, format, args);
        va_end(args);
    }

private:
    static void logArgs(LogLevel level, const char* module, const char* format, va_list args);
};

} // namespace services
