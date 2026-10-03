#pragma once

#include "app/engine.hpp"
#include "ops/httpserver.hpp"

#include <functional>
#include <string>

namespace replay
{
HttpResponse handleApi(Engine& engine, HttpRequest const& request, std::string const& indexHtml);
} // namespace replay
