#include "tally/tsl.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <sstream>
#include <utility>

namespace replay
{
namespace
{
constexpr std::uint8_t kDle = 0xfe;
constexpr std::uint8_t kStx = 0x02;
constexpr std::uint8_t kEtx = 0x03;

std::uint16_t read16(std::uint8_t const* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
} // namespace

std::vector<std::vector<std::uint8_t>> TslFramer::feed(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::vector<std::uint8_t>> packets;
    auto finish = [&] {
        if (inPacket_ && !buffer_.empty())
        {
            packets.push_back(buffer_);
        }
        buffer_.clear();
    };
    for (std::size_t i = 0; i < size; ++i)
    {
        std::uint8_t const byte = data[i];
        if (!dle_ && byte == kDle)
        {
            dle_ = true;
            continue;
        }
        bool const escaped = dle_;
        dle_ = false;
        if (escaped && byte != kDle)
        {
            // DLE/STX starts a packet, DLE/ETX ends one. A new DLE/STX also ends the previous one.
            if (byte == kStx || byte == kEtx)
            {
                finish();
                inPacket_ = byte == kStx;
            }
            continue;
        }
        if (!inPacket_)
        {
            continue;
        }
        buffer_.push_back(byte);
        // PBC counts the bytes after it: the packet is complete without waiting for DLE/ETX.
        if (buffer_.size() >= 2 && buffer_.size() == read16(buffer_.data()) + 2u)
        {
            finish();
            inPacket_ = false;
        }
    }
    return packets;
}

std::vector<std::vector<std::uint8_t>> tslDatagram(std::uint8_t const* data, std::size_t size)
{
    if (size >= 2 && data[0] == kDle && data[1] == kStx)
    {
        TslFramer framer;
        return framer.feed(data, size);
    }
    return {std::vector<std::uint8_t>(data, data + size)};
}

TslMessage parseTsl5(std::uint8_t const* body, std::size_t size)
{
    TslMessage message;
    if (body == nullptr || size < 6)
    {
        message.error = "short";
        return message;
    }
    std::uint16_t const pbc = read16(body);
    if (static_cast<std::size_t>(pbc) + 2 > size)
    {
        message.error = "pbc";
        return message;
    }
    std::size_t const end = static_cast<std::size_t>(pbc) + 2;
    bool const unicode = (body[3] & 0x01) != 0;
    if ((body[3] & 0x02) != 0)
    {
        return message;
    }
    int const screen = read16(body + 4);
    std::size_t cursor = 6;
    while (cursor + 4 <= end)
    {
        int const index = read16(body + cursor);
        std::uint16_t const control = read16(body + cursor + 2);
        cursor += 4;
        if ((control & 0x8000) != 0)
        {
            if (cursor + 2 > end)
            {
                message.error = "control-length";
                return message;
            }
            int const length = read16(body + cursor);
            cursor += 2 + static_cast<std::size_t>(length);
            continue;
        }
        if (cursor + 2 > end)
        {
            message.error = "length";
            return message;
        }
        int const length = read16(body + cursor);
        cursor += 2;
        if (cursor + static_cast<std::size_t>(length) > end)
        {
            message.error = "text";
            return message;
        }
        TallyUpdate update;
        update.screen = screen;
        update.index = index;
        update.rh = control & 0x3;
        update.text = (control >> 2) & 0x3;
        update.lh = (control >> 4) & 0x3;
        update.brightness = (control >> 6) & 0x3;
        if (unicode)
        {
            // UTF-16LE to UTF-8 (the API carries UTF-8). A lone surrogate is U+FFFD.
            for (int i = 0; i + 1 < length; i += 2)
            {
                std::uint32_t cp = read16(body + cursor + static_cast<std::size_t>(i));
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < length)
                {
                    std::uint32_t const low = read16(body + cursor + static_cast<std::size_t>(i) + 2);
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF)
                {
                    cp = 0xFFFD;
                }
                if (cp != 0)
                {
                    appendUtf8(update.textValue, cp);
                }
            }
        }
        else
        {
            // ASCII: a byte above 0x7F is not ASCII and becomes U+FFFD, so the API stays valid UTF-8.
            for (int i = 0; i < length; ++i)
            {
                std::uint8_t const byte = body[cursor + static_cast<std::size_t>(i)];
                appendUtf8(update.textValue, byte < 0x80 ? byte : 0xFFFD);
            }
            while (!update.textValue.empty() && update.textValue.back() == '\0')
            {
                update.textValue.pop_back();
            }
        }
        cursor += static_cast<std::size_t>(length);
        message.displays.push_back(std::move(update));
    }
    return message;
}

int effectiveTally(int lh, int rh, int text)
{
    if (text != 0)
    {
        return text;
    }
    if (rh != 0)
    {
        return rh;
    }
    return lh;
}

int channelForDisplay(std::string const& map, int display)
{
    if (map.empty())
    {
        return display + 1;
    }
    std::stringstream stream(map);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        auto const colon = item.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        if (std::atoi(item.c_str()) == display)
        {
            return std::atoi(item.c_str() + colon + 1);
        }
    }
    return 0;
}

TslListener::~TslListener()
{
    stop();
}

bool TslListener::start(int udpPort, int tcpPort, std::function<void(TslMessage const&)> onMessage)
{
    onMessage_ = std::move(onMessage);
    udp_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    tcp_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int const one = 1;
    ::setsockopt(tcp_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    auto bindTo = [](int fd, int port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        return fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    };
    if (!bindTo(udp_, udpPort) || !bindTo(tcp_, tcpPort) || ::listen(tcp_, 8) != 0)
    {
        stop();
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void TslListener::stop()
{
    running_.store(false);
    if (thread_.joinable())
    {
        thread_.join();
    }
    for (int* fd : {&udp_, &tcp_})
    {
        if (*fd >= 0)
        {
            ::close(*fd);
            *fd = -1;
        }
    }
}

void TslListener::run()
{
    struct Client
    {
        int fd = -1;
        TslFramer framer;
    };
    std::vector<Client> clients;
    std::vector<std::uint8_t> buffer(65536);
    auto apply = [&](std::vector<std::uint8_t> const& packet) { onMessage_(parseTsl5(packet.data(), packet.size())); };
    while (running_.load())
    {
        std::vector<pollfd> fds{{udp_, POLLIN, 0}, {tcp_, POLLIN, 0}};
        for (auto const& client : clients)
        {
            fds.push_back({client.fd, POLLIN, 0});
        }
        if (::poll(fds.data(), fds.size(), 200) <= 0)
        {
            continue;
        }
        if ((fds[0].revents & POLLIN) != 0)
        {
            auto const n = ::recv(udp_, buffer.data(), buffer.size(), 0);
            if (n > 0)
            {
                for (auto const& packet : tslDatagram(buffer.data(), static_cast<std::size_t>(n)))
                {
                    apply(packet);
                }
            }
        }
        // Backwards, so erasing a closed client keeps the earlier ones in step with fds.
        for (std::size_t i = clients.size(); i-- > 0;)
        {
            if (fds[i + 2].revents == 0)
            {
                continue;
            }
            auto const n = ::recv(clients[i].fd, buffer.data(), buffer.size(), 0);
            if (n <= 0)
            {
                ::close(clients[i].fd);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            for (auto const& packet : clients[i].framer.feed(buffer.data(), static_cast<std::size_t>(n)))
            {
                apply(packet);
            }
        }
        if ((fds[1].revents & POLLIN) != 0)
        {
            int const fd = ::accept4(tcp_, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0 && clients.size() >= 8)
            {
                ::close(fd);
            }
            else if (fd >= 0)
            {
                clients.push_back({fd, {}});
            }
        }
    }
    for (auto const& client : clients)
    {
        ::close(client.fd);
    }
}
} // namespace replay
