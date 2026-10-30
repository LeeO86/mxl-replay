#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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
// Such a request needs a Content-Length (else 411). `admit` may refuse it from its headers and length
// (a response, e.g. 413 or 507); more than `maxActive` at a time (0: no limit) get 409. A refused body
// is read and dropped, then answered, unless the client sent `Expect: 100-continue` (answered at once).
// A body that stops arriving for `stallSeconds` (0: never) is dropped with its file.
struct HttpBodyFiles
{
    std::function<bool(std::string const& method, std::string const& path)> match;
    std::string directory;
    std::function<std::optional<HttpResponse>(HttpRequest const& request, std::uint64_t length)> admit;
    std::size_t maxActive = 0;
    int stallSeconds = 30;
    // Called on the server thread when a body is accepted and starts to arrive (`length` bytes), and when one
    // is answered without the handler (`status` 400, 409, 411, the admit's answer, 500 when it cannot be
    // written) or dropped (`status` 0: it stopped arriving).
    std::function<void(HttpRequest const& request, std::uint64_t length)> started;
    std::function<void(HttpRequest const& request, std::uint64_t length, int status, std::string const& reason)> failed;
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
