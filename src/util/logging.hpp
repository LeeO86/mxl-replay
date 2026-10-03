#pragma once

#include <string>
#include <utility>
#include <vector>

namespace replay
{
enum class LogLevel
{
    Error,
    Warn,
    Info,
    Debug
};

using LogFields = std::vector<std::pair<std::string, std::string>>;

void setLogLevel(LogLevel level);
void setLogFormatJson(bool json);
LogLevel parseLogLevel(std::string const& text);
void logMessage(LogLevel level, std::string const& event, LogFields const& fields = {});

inline void logInfo(std::string const& event, LogFields const& fields = {})
{
    logMessage(LogLevel::Info, event, fields);
}
inline void logWarn(std::string const& event, LogFields const& fields = {})
{
    logMessage(LogLevel::Warn, event, fields);
}
inline void logError(std::string const& event, LogFields const& fields = {})
{
    logMessage(LogLevel::Error, event, fields);
}

std::string jsonEscape(std::string const& text);
} // namespace replay
