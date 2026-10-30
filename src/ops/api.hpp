#pragma once

#include "app/engine.hpp"
#include "ops/httpserver.hpp"

#include <functional>
#include <string>

namespace replay
{
HttpResponse handleApi(Engine& engine, HttpRequest const& request, std::string const& indexHtml);
// The upload log lines (1.8.3): `upload_started` when a body of `bytes` starts to arrive, `upload_failed` when
// it is answered with an error (`status`, 0 when dropped) before or after it arrived. `name` is the query's.
void logUploadStarted(HttpRequest const& request, std::uint64_t bytes);
void logUploadFailed(HttpRequest const& request, std::uint64_t bytes, int status, std::string const& reason);
} // namespace replay
