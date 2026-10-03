#include "media/anc.hpp"

#include "media/timebase.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace replay
{
namespace
{
std::uint16_t withParity(std::uint8_t b)
{
    int ones = 0;
    for (int i = 0; i < 8; ++i)
    {
        ones += (b >> i) & 1;
    }
    std::uint16_t w = b;
    if ((ones & 1) != 0)
    {
        w = static_cast<std::uint16_t>(w | 0x100);
    }
    else
    {
        w = static_cast<std::uint16_t>(w | 0x200);
    }
    return w;
}

class BitWriter
{
public:
    explicit BitWriter(std::vector<std::uint8_t>& out)
        : out_(out)
    {
    }
    void put(std::uint32_t value, int bits)
    {
        for (int i = bits - 1; i >= 0; --i)
        {
            int const bit = (value >> i) & 1;
            if ((bitpos_ & 7) == 0)
            {
                out_.push_back(0);
            }
            if (bit != 0)
            {
                out_.back() = static_cast<std::uint8_t>(out_.back() | (0x80 >> (bitpos_ & 7)));
            }
            ++bitpos_;
        }
    }
    void align32()
    {
        while ((bitpos_ & 31) != 0)
        {
            put(0, 1);
        }
    }

private:
    std::vector<std::uint8_t>& out_;
    std::size_t bitpos_ = 0;
};

class BitReader
{
public:
    BitReader(std::uint8_t const* p, std::size_t n)
        : p_(p)
        , n_(n)
    {
    }
    int get(int bits)
    {
        int v = 0;
        for (int i = 0; i < bits; ++i)
        {
            if (byte_ >= n_)
            {
                return -1;
            }
            int const bit = (p_[byte_] >> (7 - (bitpos_ & 7))) & 1;
            v = (v << 1) | bit;
            ++bitpos_;
            if ((bitpos_ & 7) == 0)
            {
                ++byte_;
            }
        }
        return v;
    }

private:
    std::uint8_t const* p_;
    std::size_t n_;
    std::size_t byte_ = 0;
    std::size_t bitpos_ = 0;
};

void putNibble(std::uint8_t udw[16], int index, int nibble)
{
    udw[index] = static_cast<std::uint8_t>((udw[index] & 0x0F) | ((nibble & 0x0F) << 4));
}

void setDbb1(std::uint8_t udw[16], std::uint8_t dbb)
{
    for (int i = 0; i < 8; ++i)
    {
        if (((dbb >> i) & 1) != 0)
        {
            udw[i] = static_cast<std::uint8_t>(udw[i] | 0x08);
        }
        else
        {
            udw[i] = static_cast<std::uint8_t>(udw[i] & ~0x08);
        }
    }
}

std::uint8_t getDbb1(std::uint8_t const udw[16])
{
    std::uint8_t d = 0;
    for (int i = 0; i < 8; ++i)
    {
        if ((udw[i] & 0x08) != 0)
        {
            d = static_cast<std::uint8_t>(d | (1u << i));
        }
    }
    return d;
}

int nibbleOf(std::uint8_t const udw[16], int index)
{
    return (udw[index] >> 4) & 0x0F;
}
} // namespace

std::string Timecode::format() const
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), drop ? "%02d:%02d:%02d;%02d" : "%02d:%02d:%02d:%02d", hh, mm, ss, ff);
    return buf;
}

std::vector<std::uint8_t> encodeAncGrain(AncPacket const& pkt)
{
    std::uint8_t udw[16] = {};
    putNibble(udw, 0, pkt.tc.ff % 10);
    int frameTens = (pkt.tc.ff / 10) & 0x3;
    if (pkt.tc.drop)
    {
        frameTens |= 0x4;
    }
    putNibble(udw, 2, frameTens);
    putNibble(udw, 4, pkt.tc.ss % 10);
    putNibble(udw, 6, (pkt.tc.ss / 10) & 0x7);
    putNibble(udw, 8, pkt.tc.mm % 10);
    putNibble(udw, 10, (pkt.tc.mm / 10) & 0x7);
    putNibble(udw, 12, pkt.tc.hh % 10);
    int hourTens = (pkt.tc.hh / 10) & 0x3;
    if (pkt.tc.field != 0)
    {
        hourTens |= 0x8;
    }
    putNibble(udw, 14, hourTens);
    std::uint8_t dbb = 0;
    if (pkt.kind == AtcKind::Vitc1)
    {
        dbb = 0x01;
    }
    else if (pkt.kind == AtcKind::Vitc2)
    {
        dbb = 0x02;
    }
    setDbb1(udw, dbb);

    std::vector<std::uint16_t> words;
    words.push_back(withParity(0x60));
    words.push_back(withParity(0x60));
    words.push_back(withParity(16));
    std::uint32_t sum = 0;
    auto acc = [&](std::uint16_t w) { sum = (sum + (w & 0x1FF)) & 0x1FF; };
    acc(words[0]);
    acc(words[1]);
    acc(words[2]);
    for (int i = 0; i < 16; ++i)
    {
        auto const w = withParity(udw[i]);
        words.push_back(w);
        acc(w);
    }
    std::uint16_t const cs = static_cast<std::uint16_t>((sum & 0x1FF) | (((sum & 0x100) != 0 ? 0 : 1) << 9));
    words.push_back(cs);

    std::vector<std::uint8_t> ancBytes;
    BitWriter bw(ancBytes);
    std::uint32_t const header = (0u << 31) | ((static_cast<std::uint32_t>(pkt.line) & 0x7FFu) << 20) | ((0xFFEu & 0xFFFu) << 8);
    bw.put(header, 32);
    for (auto w : words)
    {
        bw.put(w, 10);
    }
    bw.align32();

    int const f = !pkt.interlaced ? 0 : (pkt.tc.field != 0 ? 0x3 : 0x2);
    std::vector<std::uint8_t> grain(kAncGrainSize, 0);
    grain[0] = static_cast<std::uint8_t>(pkt.sequence >> 8);
    grain[1] = static_cast<std::uint8_t>(pkt.sequence);
    auto const length = static_cast<std::uint16_t>(ancBytes.size());
    grain[2] = static_cast<std::uint8_t>(length >> 8);
    grain[3] = static_cast<std::uint8_t>(length);
    grain[4] = 1;
    grain[5] = static_cast<std::uint8_t>(f << 6);
    std::memcpy(grain.data() + 8, ancBytes.data(), std::min(ancBytes.size(), grain.size() - 8));
    return grain;
}

bool decodeAncTimecode(std::uint8_t const* grain, std::size_t size, Timecode& out, AtcKind& kind)
{
    if (grain == nullptr || size < 16)
    {
        return false;
    }
    if (grain[4] < 1)
    {
        return false;
    }
    std::uint16_t const length = static_cast<std::uint16_t>((grain[2] << 8) | grain[3]);
    if (static_cast<std::size_t>(8 + length) > size)
    {
        return false;
    }
    BitReader br(grain + 8, length);
    if (br.get(32) < 0)
    {
        return false;
    }
    int const did = br.get(10);
    int const sdid = br.get(10);
    int const dc = br.get(10);
    if (did < 0 || sdid < 0 || dc < 0)
    {
        return false;
    }
    if ((did & 0xFF) != 0x60 || (sdid & 0xFF) != 0x60)
    {
        return false;
    }
    int const nwords = dc & 0xFF;
    if (nwords < 16)
    {
        return false;
    }
    std::uint8_t udw[16];
    for (int i = 0; i < 16; ++i)
    {
        int const w = br.get(10);
        if (w < 0)
        {
            return false;
        }
        udw[i] = static_cast<std::uint8_t>(w & 0xFF);
    }
    int const dbb = getDbb1(udw);
    if (dbb == 0x01)
    {
        kind = AtcKind::Vitc1;
    }
    else if (dbb == 0x02)
    {
        kind = AtcKind::Vitc2;
    }
    else
    {
        kind = AtcKind::Ltc;
    }
    int const fu = nibbleOf(udw, 0);
    int const ft = nibbleOf(udw, 2);
    int const su = nibbleOf(udw, 4);
    int const st = nibbleOf(udw, 6);
    int const mu = nibbleOf(udw, 8);
    int const mt = nibbleOf(udw, 10);
    int const hu = nibbleOf(udw, 12);
    int const ht = nibbleOf(udw, 14);
    out = {};
    out.ff = (ft & 0x3) * 10 + (fu & 0xF);
    out.ss = (st & 0x7) * 10 + (su & 0xF);
    out.mm = (mt & 0x7) * 10 + (mu & 0xF);
    out.hh = (ht & 0x3) * 10 + (hu & 0xF);
    out.drop = (ft & 0x4) != 0;
    out.field = (ht & 0x8) != 0 ? 1 : 0;
    int const f = (grain[5] >> 6) & 0x3;
    if (f == 0x3)
    {
        out.field = 1;
    }
    else if (f == 0x2)
    {
        out.field = 0;
    }
    return true;
}

Timecode timecodeFromTai(std::uint64_t timestampNs, int rateNum, int rateDen, bool dropFrame)
{
    auto const text = formatTimecode(timestampNs, rateNum, rateDen, dropFrame);
    Timecode tc;
    tc.drop = dropFrame;
    if (text.size() >= 11)
    {
        tc.hh = std::stoi(text.substr(0, 2));
        tc.mm = std::stoi(text.substr(3, 2));
        tc.ss = std::stoi(text.substr(6, 2));
        tc.ff = std::stoi(text.substr(9, 2));
    }
    return tc;
}
} // namespace replay
