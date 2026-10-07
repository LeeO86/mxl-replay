#include "util/json.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace replay
{
namespace
{
class JsonReader
{
public:
    explicit JsonReader(std::string const& text)
        : text_(text)
    {
    }

    bool parse(Json& out)
    {
        if (!value(out, 0))
        {
            return false;
        }
        skipSpace();
        return pos_ == text_.size();
    }

private:
    void skipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
        {
            ++pos_;
        }
    }

    bool consume(char c)
    {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c)
        {
            ++pos_;
            return true;
        }
        return false;
    }

    bool word(std::string_view literal)
    {
        if (text_.compare(pos_, literal.size(), literal) != 0)
        {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    bool value(Json& out, int depth)
    {
        skipSpace();
        if (pos_ >= text_.size() || depth > 32)
        {
            return false;
        }
        char const c = text_[pos_];
        if (c == '{')
        {
            ++pos_;
            out.kind = Json::Kind::Object;
            if (consume('}'))
            {
                return true;
            }
            do
            {
                skipSpace();
                std::string key;
                Json member;
                if (!string(key) || !consume(':') || !value(member, depth + 1))
                {
                    return false;
                }
                out.members.emplace_back(std::move(key), std::move(member));
            } while (consume(','));
            return consume('}');
        }
        if (c == '[')
        {
            ++pos_;
            out.kind = Json::Kind::Array;
            if (consume(']'))
            {
                return true;
            }
            do
            {
                Json item;
                if (!value(item, depth + 1))
                {
                    return false;
                }
                out.items.push_back(std::move(item));
            } while (consume(','));
            return consume(']');
        }
        if (c == '"')
        {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (c == 't' || c == 'f')
        {
            out.kind = Json::Kind::Bool;
            out.boolean = c == 't';
            return word(out.boolean ? "true" : "false");
        }
        if (c == 'n')
        {
            return word("null");
        }
        return number(out);
    }

    bool hex4(unsigned& code)
    {
        if (pos_ + 4 > text_.size())
        {
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i)
        {
            char const h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9')
            {
                code |= static_cast<unsigned>(h - '0');
            }
            else if (h >= 'a' && h <= 'f')
            {
                code |= static_cast<unsigned>(h - 'a' + 10);
            }
            else if (h >= 'A' && h <= 'F')
            {
                code |= static_cast<unsigned>(h - 'A' + 10);
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    static void utf8(unsigned code, std::string& out)
    {
        if (code < 0x80)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else if (code < 0x10000)
        {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    bool string(std::string& out)
    {
        if (pos_ >= text_.size() || text_[pos_] != '"')
        {
            return false;
        }
        ++pos_;
        while (pos_ < text_.size())
        {
            char const c = text_[pos_++];
            if (c == '"')
            {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20)
            {
                return false;
            }
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size())
            {
                return false;
            }
            char const escaped = text_[pos_++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                out.push_back(escaped);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
            {
                unsigned code = 0;
                if (!hex4(code))
                {
                    return false;
                }
                if (code >= 0xd800 && code < 0xdc00)
                {
                    unsigned low = 0;
                    if (!word("\\u") || !hex4(low) || low < 0xdc00 || low >= 0xe000)
                    {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(code, out);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool number(Json& out)
    {
        auto const start = pos_;
        auto const digits = [&] {
            auto const from = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                ++pos_;
            }
            return pos_ > from;
        };
        if (pos_ < text_.size() && text_[pos_] == '-')
        {
            ++pos_;
        }
        if (!digits())
        {
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == '.')
        {
            ++pos_;
            if (!digits())
            {
                return false;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
            {
                ++pos_;
            }
            if (!digits())
            {
                return false;
            }
        }
        out.kind = Json::Kind::Number;
        out.text = text_.substr(start, pos_ - start);
        return true;
    }

    std::string const& text_;
    std::size_t pos_ = 0;
};

} // namespace

Json const* Json::get(std::string const& key) const
{
    for (auto const& member : members)
    {
        if (member.first == key)
        {
            return &member.second;
        }
    }
    return nullptr;
}

bool parseJson(std::string const& text, Json& out)
{
    JsonReader reader(text);
    return reader.parse(out);
}

std::optional<double> jsonDouble(Json const& value)
{
    if ((value.kind != Json::Kind::Number && value.kind != Json::Kind::String) || value.text.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    double const result = std::strtod(value.text.c_str(), &end);
    if (errno != 0 || end != value.text.c_str() + value.text.size() || !std::isfinite(result))
    {
        return std::nullopt;
    }
    return result;
}

std::optional<std::int64_t> jsonInt(Json const& value)
{
    if ((value.kind != Json::Kind::Number && value.kind != Json::Kind::String) || value.text.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    long long const result = std::strtoll(value.text.c_str(), &end, 10);
    if (errno != 0 || end != value.text.c_str() + value.text.size())
    {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(result);
}

std::optional<std::uint64_t> jsonUnsigned(Json const& value)
{
    if ((value.kind != Json::Kind::Number && value.kind != Json::Kind::String) || value.text.empty() || value.text.front() == '-')
    {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long const result = std::strtoull(value.text.c_str(), &end, 10);
    if (errno != 0 || end != value.text.c_str() + value.text.size())
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(result);
}

std::optional<bool> jsonBool(Json const& value)
{
    if (value.kind == Json::Kind::Bool)
    {
        return value.boolean;
    }
    if (value.text == "true" || value.text == "1")
    {
        return true;
    }
    if (value.text == "false" || value.text == "0")
    {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> jsonText(Json const& value)
{
    if (value.kind != Json::Kind::String)
    {
        return std::nullopt;
    }
    return value.text;
}
} // namespace replay
