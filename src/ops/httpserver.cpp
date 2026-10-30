#include "ops/httpserver.hpp"

#include "config/config.hpp"
#include "util/json.hpp"
#include "util/logging.hpp"
#include "util/sha1.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <cctype>
#include <cstdlib>
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
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 507:
        return "Insufficient Storage";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}
std::string responseText(HttpResponse const& response)
{
    std::string extra;
    for (auto const& [name, value] : response.headers)
    {
        extra += "\r\n" + name + ": " + value;
    }
    return "HTTP/1.1 " + std::to_string(response.status) + " " + statusText(response.status) + "\r\nContent-Type: " + response.contentType +
           "\r\nContent-Length: " + std::to_string(response.body.size()) +
           (response.cacheControl.empty() ? std::string{} : "\r\nCache-Control: " + response.cacheControl) + extra + "\r\nConnection: close\r\n\r\n" +
           response.body;
}

HttpResponse errorResponse(int status, std::string const& message)
{
    HttpResponse response;
    response.status = status;
    response.contentType = "application/json";
    response.body = "{\"error\":\"" + jsonEscape(message) + "\"}";
    return response;
}

// The "error" of a JSON error answer; its body when it has none.
std::string errorText(HttpResponse const& response)
{
    Json json;
    if (parseJson(response.body, json))
    {
        if (auto const* error = json.get("error"); error != nullptr && error->kind == Json::Kind::String)
        {
            return error->text;
        }
    }
    return response.body;
}

// A request body written to a file as it arrives (HttpBodyFiles). Removes the file unless it was handed on.
struct BodyFile
{
    HttpRequest request;
    int fd = -1;
    std::string path;
    std::uint64_t remaining = 0;
    std::uint64_t length = 0; // the Content-Length
    // Refused (HttpBodyFiles::admit, busy): the body is read and dropped, then this is the answer.
    std::optional<HttpResponse> refusal;
    std::string error;

    BodyFile() = default;
    BodyFile(BodyFile const&) = delete;
    BodyFile& operator=(BodyFile const&) = delete;
    ~BodyFile()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
        if (!path.empty())
        {
            ::unlink(path.c_str());
        }
    }

    // Keeps reading past a write error or the size limit, so the client gets the answer, not a reset.
    void write(char const* data, std::size_t size)
    {
        remaining -= size;
        while (fd >= 0 && size > 0)
        {
            auto const n = ::write(fd, data, size);
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            if (n <= 0)
            {
                error = std::string("the upload could not be written: ") + std::strerror(errno);
                ::close(fd);
                fd = -1;
                ::unlink(path.c_str());
                path.clear();
                return;
            }
            data += n;
            size -= static_cast<std::size_t>(n);
        }
    }
};
} // namespace

struct HttpServer::Impl
{
    int listenFd = -1;
    int boundPort = 0;
    std::atomic<bool> run{false};
    std::thread thread;
    HttpHandler handler;
    HttpBodyFiles bodyFiles;
    struct Conn
    {
        int fd = -1;
        bool websocket = false;
        std::string buffer;
        bool closing = false; // shut down by broadcast; the loop closes it
        std::shared_ptr<BodyFile> body; // set while a body goes to a file
        std::chrono::steady_clock::time_point lastData = std::chrono::steady_clock::now();
    };
    std::mutex connMu;
    std::vector<Conn> conns;
    // Handlers of requests with a body file run here, so a long one (an upload) does not hold the loop.
    struct Worker
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex workerMu;
    std::vector<Worker> workers;

    HttpResponse handle(HttpRequest const& request)
    {
        // A handler that throws answers 500; it must not end the process.
        try
        {
            return handler ? handler(request) : HttpResponse{};
        }
        catch (std::exception const& ex)
        {
            return errorResponse(500, ex.what());
        }
    }

    void startWorker(int fd, HttpRequest request)
    {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard lock{workerMu};
        workers.push_back(Worker{std::thread([this, fd, request = std::move(request), done] {
            auto const message = responseText(handle(request));
            sendAll(fd, message.data(), message.size());
            ::close(fd);
            ::unlink(request.bodyPath.c_str());
            done->store(true);
        }),
            done});
    }

    // Requests with a body file in progress: received or handled.
    std::size_t activeBodies()
    {
        std::size_t count = 0;
        for (auto const& conn : conns)
        {
            count += conn.body && !conn.body->refusal ? 1 : 0;
        }
        std::lock_guard lock{workerMu};
        for (auto const& worker : workers)
        {
            count += worker.done->load() ? 0 : 1;
        }
        return count;
    }

    void reapWorkers(bool all)
    {
        std::lock_guard lock{workerMu};
        for (auto it = workers.begin(); it != workers.end();)
        {
            if (all || it->done->load())
            {
                it->thread.join();
                it = workers.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

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
                    // A client that stops reading cannot hold the loop for long.
                    timeval timeout{2, 0};
                    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                    std::lock_guard lock{connMu};
                    conns.push_back(Conn{fd, false, {}, false});
                }
            }
            reapWorkers(false);
            struct Job
            {
                int fd = -1;
                HttpRequest request;
                bool fixed = false; // `response` is the answer (no handler)
                HttpResponse response;
            };
            std::vector<Job> jobs;
            std::vector<int> drop;
            std::vector<int> handedOff; // owned by a worker now: removed from conns, not closed
            // A body file is complete: its handler runs on a worker, or a refusal is sent.
            auto finishBody = [&](Conn& conn) {
                auto body = std::move(conn.body);
                if (!body->refusal && !body->error.empty() && bodyFiles.failed)
                {
                    bodyFiles.failed(body->request, body->length, 500, body->error);
                }
                if (body->refusal || !body->error.empty())
                {
                    jobs.push_back(Job{conn.fd, {}, true, body->refusal ? *body->refusal : errorResponse(500, body->error)});
                    return;
                }
                ::close(body->fd);
                body->fd = -1;
                body->request.bodyPath = body->path;
                body->path.clear(); // the worker removes it
                startWorker(conn.fd, std::move(body->request));
                handedOff.push_back(conn.fd);
            };
            {
                std::unique_lock lock{connMu};
                auto const now = std::chrono::steady_clock::now();
                for (std::size_t i = 1; i < fds.size() && i - 1 < conns.size(); ++i)
                {
                    if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    {
                        continue;
                    }
                    auto& conn = conns[i - 1];
                    char buf[65536];
                    auto const n = ::recv(conn.fd, buf, sizeof(buf), 0);
                    if (n <= 0)
                    {
                        drop.push_back(conn.fd);
                        continue;
                    }
                    conn.lastData = now;
                    if (conn.body)
                    {
                        // Only this thread changes `conns`: the disk write does not hold up broadcast().
                        auto const body = conn.body;
                        lock.unlock();
                        body->write(buf, std::min<std::size_t>(static_cast<std::size_t>(n), body->remaining));
                        lock.lock();
                        if (conn.body->remaining == 0)
                        {
                            finishBody(conn);
                        }
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
                    bool badLength = false;
                    bool lengthSeen = false;
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
                            // Digits only, and the same value when repeated (RFC 9110 §8.6).
                            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                            {
                                value.pop_back();
                            }
                            bool const digits = !value.empty() && value.size() <= 15 &&
                                                std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
                            auto const parsed = digits ? static_cast<std::size_t>(std::stoull(value)) : 0;
                            badLength = badLength || !digits || (lengthSeen && parsed != bodyLen);
                            bodyLen = parsed;
                            lengthSeen = true;
                        }
                    }
                    bool const bodyFile = bodyFiles.match && bodyFiles.match(request.method, request.path);
                    // A body-file request answered here: the answer, and the `failed` hook.
                    auto const refuse = [&](HttpResponse const& response, std::string const& reason) {
                        if (bodyFile && bodyFiles.failed)
                        {
                            bodyFiles.failed(request, bodyLen, response.status, reason);
                        }
                        jobs.push_back(Job{conn.fd, {}, true, response});
                        conn.buffer.clear();
                    };
                    if (badLength)
                    {
                        refuse(errorResponse(400, "invalid Content-Length"), "invalid Content-Length");
                        continue;
                    }
                    if (bodyFile && request.headers.count("transfer-encoding") != 0)
                    {
                        static char const kChunked[] = "the upload needs a Content-Length (no chunked transfer)";
                        refuse(errorResponse(411, kChunked), kChunked);
                        continue;
                    }
                    if (bodyLen > 0 && bodyFile)
                    {
                        // To a file in bodyFiles.directory as it arrives, not into memory (1.8.2).
                        auto body = std::make_shared<BodyFile>();
                        body->remaining = bodyLen;
                        if (bodyFiles.maxActive > 0 && activeBodies() >= bodyFiles.maxActive)
                        {
                            body->refusal = errorResponse(409, "an upload is already running; try again when it is stored");
                        }
                        else if (bodyFiles.admit)
                        {
                            body->refusal = bodyFiles.admit(request, bodyLen);
                        }
                        auto const expect = request.headers.find("expect");
                        bool const continues = expect != request.headers.end() && lower(expect->second) == "100-continue";
                        if (body->refusal && bodyFiles.failed)
                        {
                            bodyFiles.failed(request, bodyLen, body->refusal->status, errorText(*body->refusal));
                        }
                        if (continues && body->refusal)
                        {
                            // The client waits for 100 Continue: answer now, it does not send the body.
                            jobs.push_back(Job{conn.fd, {}, true, *body->refusal});
                            conn.buffer.clear();
                            continue;
                        }
                        if (continues)
                        {
                            static char const kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
                            sendAll(conn.fd, kContinue, sizeof(kContinue) - 1);
                        }
                        if (!body->refusal)
                        {
                            std::string pattern = bodyFiles.directory + "/body-XXXXXX";
                            body->fd = ::mkstemp(pattern.data());
                            if (body->fd < 0)
                            {
                                body->error = std::string("the upload could not be written: ") + std::strerror(errno);
                            }
                            else
                            {
                                body->path = pattern;
                                if (bodyFiles.started)
                                {
                                    bodyFiles.started(request, bodyLen);
                                }
                            }
                        }
                        body->length = bodyLen;
                        body->request = std::move(request);
                        auto const have = std::min<std::size_t>(conn.buffer.size() - (headerEnd + 4), bodyLen);
                        body->write(conn.buffer.data() + headerEnd + 4, have);
                        conn.buffer.clear();
                        conn.body = std::move(body);
                        if (conn.body->remaining == 0)
                        {
                            finishBody(conn);
                        }
                        continue;
                    }
                    if (conn.buffer.size() < headerEnd + 4 + bodyLen)
                    {
                        continue;
                    }
                    request.body = conn.buffer.substr(headerEnd + 4, bodyLen);
                    conn.buffer.erase(0, headerEnd + 4 + bodyLen);
                    jobs.push_back(Job{conn.fd, std::move(request)});
                }
                // A body that stopped arriving: the connection is closed and its file removed.
                for (auto const& conn : conns)
                {
                    if (conn.body && bodyFiles.stallSeconds > 0 && now - conn.lastData > std::chrono::seconds(bodyFiles.stallSeconds) &&
                        std::find(drop.begin(), drop.end(), conn.fd) == drop.end())
                    {
                        logWarn("upload_stalled", {{"path", conn.body->request.path}, {"remaining_bytes", std::to_string(conn.body->remaining)}});
                        if (!conn.body->refusal && bodyFiles.failed)
                        {
                            bodyFiles.failed(conn.body->request, conn.body->length, 0, "the body stopped arriving");
                        }
                        drop.push_back(conn.fd);
                    }
                }
            }
            std::vector<int> upgraded;
            for (auto const& job : jobs)
            {
                HttpResponse response = job.fixed ? job.response : handle(job.request);
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
                auto const message = responseText(response);
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
                            [&](Conn const& conn) {
                                return std::find(drop.begin(), drop.end(), conn.fd) != drop.end() ||
                                       std::find(handedOff.begin(), handedOff.end(), conn.fd) != handedOff.end();
                            }),
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

void HttpServer::start(int port, HttpHandler handler, HttpBodyFiles bodyFiles)
{
    stop();
    impl_->handler = std::move(handler);
    impl_->bodyFiles = std::move(bodyFiles);
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
        throw StartupError(75, "cannot bind WEB_PORT " + std::to_string(port));
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
    impl_->reapWorkers(true);
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

int httpGetStatus(std::string const& host, int port, std::string const& path, int timeoutMs)
{
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        return 0;
    }
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return 0;
    }
    auto const request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (!sendAll(fd, request.data(), request.size()))
    {
        ::close(fd);
        return 0;
    }
    std::string response;
    char buf[1024];
    while (response.size() < 64)
    {
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        response.append(buf, buf + n);
        if (response.find("\r\n") != std::string::npos)
        {
            break;
        }
    }
    ::close(fd);
    if (response.rfind("HTTP/", 0) != 0)
    {
        return 0;
    }
    auto const space = response.find(' ');
    if (space == std::string::npos)
    {
        return 0;
    }
    return std::atoi(response.c_str() + space + 1);
}

void HttpServer::broadcast(std::string const& text)
{
    std::vector<std::uint8_t> frame;
    frame.push_back(0x81);
    if (text.size() < 126)
    {
        frame.push_back(static_cast<std::uint8_t>(text.size()));
    }
    else if (text.size() < 65536)
    {
        frame.push_back(126);
        frame.push_back(static_cast<std::uint8_t>((text.size() >> 8) & 0xff));
        frame.push_back(static_cast<std::uint8_t>(text.size() & 0xff));
    }
    else
    {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            frame.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(text.size()) >> shift) & 0xff));
        }
    }
    frame.insert(frame.end(), text.begin(), text.end());
    std::lock_guard lock{impl_->connMu};
    for (auto& conn : impl_->conns)
    {
        if (!conn.websocket || conn.closing)
        {
            continue;
        }
        // Never wait for a client: one that cannot take the whole message now (asleep, on a slow
        // link) is shut down, the loop closes it, and its UI reconnects. A message cut in half
        // would break the stream anyway.
        std::size_t sent = 0;
        while (sent < frame.size())
        {
            auto const n = ::send(conn.fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n <= 0)
            {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
        if (sent < frame.size())
        {
            conn.closing = true;
            ::shutdown(conn.fd, SHUT_RDWR);
        }
    }
}
} // namespace replay
