#include "util/sha1.hpp"

#include <cstring>
#include <vector>

namespace replay
{
namespace
{
std::uint32_t rotl(std::uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}
} // namespace

std::array<std::uint8_t, 20> sha1(std::uint8_t const* data, std::size_t size)
{
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    std::uint64_t const bitLen = static_cast<std::uint64_t>(size) * 8ULL;
    std::size_t const padded = ((size + 9 + 63) / 64) * 64;
    std::vector<std::uint8_t> buf(padded, 0);
    if (size > 0)
    {
        std::memcpy(buf.data(), data, size);
    }
    buf[size] = 0x80;
    for (int i = 0; i < 8; ++i)
    {
        buf[padded - 1 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bitLen >> (8 * i));
    }
    for (std::size_t offset = 0; offset < padded; offset += 64)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (std::uint32_t(buf[offset + static_cast<std::size_t>(i) * 4]) << 24) |
                   (std::uint32_t(buf[offset + static_cast<std::size_t>(i) * 4 + 1]) << 16) |
                   (std::uint32_t(buf[offset + static_cast<std::size_t>(i) * 4 + 2]) << 8) |
                   std::uint32_t(buf[offset + static_cast<std::size_t>(i) * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
        {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;
        for (int i = 0; i < 80; ++i)
        {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            std::uint32_t const temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    std::array<std::uint8_t, 20> out{};
    std::uint32_t const hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i)
    {
        out[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(hs[i] >> 24);
        out[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(hs[i] >> 16);
        out[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(hs[i] >> 8);
        out[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(hs[i]);
    }
    return out;
}

std::array<std::uint8_t, 20> sha1(std::string_view data)
{
    return sha1(reinterpret_cast<std::uint8_t const*>(data.data()), data.size());
}

std::string base64Encode(std::uint8_t const* data, std::size_t size)
{
    static char const table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        unsigned const b0 = data[i];
        unsigned const b1 = i + 1 < size ? data[i + 1] : 0;
        unsigned const b2 = i + 2 < size ? data[i + 2] : 0;
        unsigned const n = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? table[n & 63] : '=');
    }
    return out;
}
} // namespace replay
