#include "util/logging.hpp"

#include <iostream>
#include <mutex>

namespace replay
{
namespace
{
LogLevel gLevel = LogLevel::Info;
bool gJson = true;
std::mutex gMu;

char const* levelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:
        return "error";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Info:
        return "info";
    case LogLevel::Debug:
        return "debug";
    }
    return "info";
}
} // namespace

void setLogLevel(LogLevel level)
{
    gLevel = level;
}

void setLogFormatJson(bool json)
{
    gJson = json;
}

LogLevel parseLogLevel(std::string const& text)
{
    if (text == "error")
    {
        return LogLevel::Error;
    }
    if (text == "warn" || text == "warning")
    {
        return LogLevel::Warn;
    }
    if (text == "debug")
    {
        return LogLevel::Debug;
    }
    return LogLevel::Info;
}

std::string jsonEscape(std::string const& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

void logMessage(LogLevel level, std::string const& event, LogFields const& fields)
{
    if (static_cast<int>(level) > static_cast<int>(gLevel))
    {
        return;
    }
    std::lock_guard lock{gMu};
    if (!gJson)
    {
        std::cerr << levelName(level) << ' ' << event;
        for (auto const& field : fields)
        {
            std::cerr << ' ' << field.first << '=' << field.second;
        }
        std::cerr << '\n';
        return;
    }
    std::cerr << "{\"level\":\"" << levelName(level) << "\",\"event\":\"" << jsonEscape(event) << '"';
    for (auto const& field : fields)
    {
        std::cerr << ",\"" << jsonEscape(field.first) << "\":\"" << jsonEscape(field.second) << '"';
    }
    std::cerr << "}\n";
}
} // namespace replay
