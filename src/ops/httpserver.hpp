#pragma once

#include <functional>
#include <map>
#include <string>

namespace replay
{
struct HttpRequest
{
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct HttpResponse
{
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    bool websocket = false;
};

using HttpHandler = std::function<HttpResponse(HttpRequest const&)>;

[[nodiscard]] int httpGetStatus(std::string const& host, int port, std::string const& path, int timeoutMs);

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();
    HttpServer(HttpServer const&) = delete;
    HttpServer& operator=(HttpServer const&) = delete;

    void start(int port, HttpHandler handler);
    void stop();
    [[nodiscard]] int port() const;
    void broadcast(std::string const& text);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace replay
