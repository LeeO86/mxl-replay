#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace replay
{
struct HttpRequest
{
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;
    // A body that went to a file (HttpBodyFiles): `body` is empty, the file is removed after the handler.
    std::string bodyPath;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    // Sent as Cache-Control when not empty.
    std::string cacheControl;
    // More header lines (the widget routes' CSP, CORS).
    std::vector<std::pair<std::string, std::string>> headers;
    bool websocket = false;
};

using HttpHandler = std::function<HttpResponse(HttpRequest const&)>;

// Requests whose body is written to a file in `directory` as it arrives, instead of memory (1.8.2:
// uploads). Their handler runs on a thread of its own, so the other requests are answered meanwhile.
// A body larger than `maxBytes` (0: no limit) is read and dropped, and answered 413.
struct HttpBodyFiles
{
    std::function<bool(std::string const& method, std::string const& path)> match;
    std::string directory;
    std::uint64_t maxBytes = 0;
};

[[nodiscard]] int httpGetStatus(std::string const& host, int port, std::string const& path, int timeoutMs);

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();
    HttpServer(HttpServer const&) = delete;
    HttpServer& operator=(HttpServer const&) = delete;

    void start(int port, HttpHandler handler, HttpBodyFiles bodyFiles = {});
    void stop();
    [[nodiscard]] int port() const;
    void broadcast(std::string const& text);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace replay
