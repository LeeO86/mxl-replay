#include "ops/httpserver.hpp"

#include "util/sha1.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace replay
{
namespace
{
std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool sendAll(int fd, char const* data, std::size_t size)
{
    std::size_t sent = 0;
    while (sent < size)
    {
        auto const n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string statusText(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}
} // namespace

struct HttpServer::Impl
{
    int listenFd = -1;
    int boundPort = 0;
    std::atomic<bool> run{false};
    std::thread thread;
    HttpHandler handler;
    struct Conn
    {
        int fd = -1;
        bool websocket = false;
        std::string buffer;
    };
    std::mutex connMu;
    std::vector<Conn> conns;

    void loop()
    {
        while (run.load())
        {
            std::vector<pollfd> fds;
            fds.push_back(pollfd{listenFd, POLLIN, 0});
            {
                std::lock_guard lock{connMu};
                for (auto const& conn : conns)
                {
                    fds.push_back(pollfd{conn.fd, POLLIN, 0});
                }
            }
            if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), 200) < 0)
            {
                continue;
            }
            if (!fds.empty() && (fds[0].revents & POLLIN) != 0)
            {
                int const fd = ::accept(listenFd, nullptr, nullptr);
                if (fd >= 0)
                {
                    std::lock_guard lock{connMu};
                    conns.push_back(Conn{fd, false, {}});
                }
            }
            struct Job
            {
                int fd = -1;
                HttpRequest request;
            };
            std::vector<Job> jobs;
            std::vector<int> drop;
            {
                std::lock_guard lock{connMu};
                for (std::size_t i = 1; i < fds.size() && i - 1 < conns.size(); ++i)
                {
                    if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    {
                        continue;
                    }
                    auto& conn = conns[i - 1];
                    char buf[8192];
                    auto const n = ::recv(conn.fd, buf, sizeof(buf), 0);
                    if (n <= 0)
                    {
                        drop.push_back(conn.fd);
                        continue;
                    }
                    conn.buffer.append(buf, buf + n);
                    if (conn.websocket)
                    {
                        if (!conn.buffer.empty() && (static_cast<unsigned char>(conn.buffer[0]) & 0x0f) == 0x8)
                        {
                            drop.push_back(conn.fd);
                        }
                        conn.buffer.clear();
                        continue;
                    }
                    auto const headerEnd = conn.buffer.find("\r\n\r\n");
                    if (headerEnd == std::string::npos)
                    {
                        continue;
                    }
                    std::string const head = conn.buffer.substr(0, headerEnd);
                    std::size_t bodyLen = 0;
                    HttpRequest request;
                    std::istringstream lines(head);
                    std::string requestLine;
                    std::getline(lines, requestLine);
                    if (!requestLine.empty() && requestLine.back() == '\r')
                    {
                        requestLine.pop_back();
                    }
                    std::istringstream rl(requestLine);
                    std::string target;
                    rl >> request.method >> target;
                    auto const q = target.find('?');
                    if (q == std::string::npos)
                    {
                        request.path = target;
                    }
                    else
                    {
                        request.path = target.substr(0, q);
                        request.query = target.substr(q + 1);
                    }
                    std::string line;
                    while (std::getline(lines, line))
                    {
                        if (!line.empty() && line.back() == '\r')
                        {
                            line.pop_back();
                        }
                        auto const colon = line.find(':');
                        if (colon == std::string::npos)
                        {
                            continue;
                        }
                        auto key = lower(line.substr(0, colon));
                        auto value = line.substr(colon + 1);
                        while (!value.empty() && value.front() == ' ')
                        {
                            value.erase(value.begin());
                        }
                        request.headers[key] = value;
                        if (key == "content-length")
                        {
                            bodyLen = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
                        }
                    }
                    if (conn.buffer.size() < headerEnd + 4 + bodyLen)
                    {
                        continue;
                    }
                    request.body = conn.buffer.substr(headerEnd + 4, bodyLen);
                    conn.buffer.erase(0, headerEnd + 4 + bodyLen);
                    jobs.push_back(Job{conn.fd, std::move(request)});
                }
            }
            std::vector<int> upgraded;
            for (auto const& job : jobs)
            {
                HttpResponse response = handler ? handler(job.request) : HttpResponse{};
                if (response.websocket)
                {
                    auto const keyIt = job.request.headers.find("sec-websocket-key");
                    auto const key = keyIt == job.request.headers.end() ? std::string{} : keyIt->second;
                    auto const digest = sha1(std::string(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
                    auto const accept = base64Encode(digest.data(), digest.size());
                    std::string upgrade = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept +
                                          "\r\n\r\n";
                    if (!sendAll(job.fd, upgrade.data(), upgrade.size()))
                    {
                        drop.push_back(job.fd);
                    }
                    else
                    {
                        upgraded.push_back(job.fd);
                    }
                    continue;
                }
                std::string message = "HTTP/1.1 " + std::to_string(response.status) + " " + statusText(response.status) + "\r\nContent-Type: " +
                                      response.contentType + "\r\nContent-Length: " + std::to_string(response.body.size()) + "\r\nConnection: close\r\n\r\n" +
                                      response.body;
                sendAll(job.fd, message.data(), message.size());
                drop.push_back(job.fd);
            }
            std::lock_guard lock{connMu};
            for (auto& conn : conns)
            {
                if (std::find(upgraded.begin(), upgraded.end(), conn.fd) != upgraded.end())
                {
                    conn.websocket = true;
                }
            }
            for (int fd : drop)
            {
                ::close(fd);
            }
            conns.erase(std::remove_if(conns.begin(), conns.end(),
                            [&](Conn const& conn) { return std::find(drop.begin(), drop.end(), conn.fd) != drop.end(); }),
                conns.end());
        }
    }
};

HttpServer::HttpServer()
    : impl_(new Impl)
{
}

HttpServer::~HttpServer()
{
    stop();
    delete impl_;
}

void HttpServer::start(int port, HttpHandler handler)
{
    stop();
    impl_->handler = std::move(handler);
    impl_->listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listenFd < 0)
    {
        throw std::runtime_error("socket failed");
    }
    int const one = 1;
    ::setsockopt(impl_->listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(impl_->listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(impl_->listenFd);
        impl_->listenFd = -1;
        throw std::runtime_error("bind failed");
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(impl_->listenFd, reinterpret_cast<sockaddr*>(&bound), &len);
    impl_->boundPort = ntohs(bound.sin_port);
    if (::listen(impl_->listenFd, 64) != 0)
    {
        throw std::runtime_error("listen failed");
    }
    impl_->run.store(true);
    impl_->thread = std::thread([this] { impl_->loop(); });
}

void HttpServer::stop()
{
    if (impl_ == nullptr || !impl_->run.load())
    {
        if (impl_ != nullptr && impl_->listenFd >= 0)
        {
            ::close(impl_->listenFd);
            impl_->listenFd = -1;
        }
        return;
    }
    impl_->run.store(false);
    if (impl_->listenFd >= 0)
    {
        ::shutdown(impl_->listenFd, SHUT_RDWR);
        ::close(impl_->listenFd);
        impl_->listenFd = -1;
    }
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
    std::lock_guard lock{impl_->connMu};
    for (auto& conn : impl_->conns)
    {
        ::close(conn.fd);
    }
    impl_->conns.clear();
}

int HttpServer::port() const
{
    return impl_->boundPort;
}

void HttpServer::broadcast(std::string const& text)
{
    std::vector<std::uint8_t> frame;
    frame.push_back(0x81);
    if (text.size() < 126)
    {
        frame.push_back(static_cast<std::uint8_t>(text.size()));
    }
    else
    {
        frame.push_back(126);
        frame.push_back(static_cast<std::uint8_t>((text.size() >> 8) & 0xff));
        frame.push_back(static_cast<std::uint8_t>(text.size() & 0xff));
    }
    frame.insert(frame.end(), text.begin(), text.end());
    std::lock_guard lock{impl_->connMu};
    for (auto& conn : impl_->conns)
    {
        if (conn.websocket)
        {
            sendAll(conn.fd, reinterpret_cast<char const*>(frame.data()), frame.size());
        }
    }
}
} // namespace replay
