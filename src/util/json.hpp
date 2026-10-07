#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace replay
{
// A parsed JSON value (request bodies, imported configuration). A number keeps its literal:
// TAI nanoseconds do not fit a double.
struct Json
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text; // a string's value, or a number's literal
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    [[nodiscard]] Json const* get(std::string const& key) const;
};

// False when `text` is not exactly one JSON value.
[[nodiscard]] bool parseJson(std::string const& text, Json& out);

// Numbers may also come as strings ("0.5"), as earlier clients sent them.
[[nodiscard]] std::optional<double> jsonDouble(Json const& value);
[[nodiscard]] std::optional<std::int64_t> jsonInt(Json const& value);
[[nodiscard]] std::optional<std::uint64_t> jsonUnsigned(Json const& value);
// true/false, or 1/0 and "true"/"false".
[[nodiscard]] std::optional<bool> jsonBool(Json const& value);
// A JSON string only.
[[nodiscard]] std::optional<std::string> jsonText(Json const& value);
} // namespace replay
