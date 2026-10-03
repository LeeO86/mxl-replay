#include "ops/metrics.hpp"

#include <algorithm>
#include <sstream>

namespace replay
{
std::string Metrics::labelText(Labels labels)
{
    if (labels.empty())
    {
        return {};
    }
    std::sort(labels.begin(), labels.end());
    std::ostringstream out;
    out << '{';
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        out << labels[i].first << "=\"" << labels[i].second << '"';
    }
    out << '}';
    return out.str();
}

std::string Metrics::key(std::string const& name, Labels const& labels)
{
    return name + labelText(labels);
}

void Metrics::inc(std::string const& name, Labels const& labels, double value)
{
    std::lock_guard lock{mutex_};
    series_[key(name, labels)].value += value;
}

void Metrics::set(std::string const& name, Labels const& labels, double value)
{
    std::lock_guard lock{mutex_};
    auto& series = series_[key(name, labels)];
    series.gauge = true;
    series.value = value;
}

void Metrics::observe(std::string const& name, Labels const& labels, double seconds)
{
    std::lock_guard lock{mutex_};
    auto& hist = histograms_[key(name, labels)];
    if (hist.buckets.empty())
    {
        hist.buckets.assign(bounds_.size(), 0);
    }
    ++hist.count;
    hist.sum += seconds;
    for (std::size_t i = 0; i < bounds_.size(); ++i)
    {
        if (seconds <= bounds_[i])
        {
            ++hist.buckets[i];
        }
    }
}

std::string Metrics::render() const
{
    std::lock_guard lock{mutex_};
    std::ostringstream out;
    out << "# TYPE mxl_replay_info gauge\n";
    for (auto const& [name, series] : series_)
    {
        out << "mxl_replay_" << name << ' ' << series.value << '\n';
    }
    for (auto const& [name, hist] : histograms_)
    {
        auto const brace = name.find('{');
        auto const base = name.substr(0, brace);
        std::string extra;
        if (brace != std::string::npos && name.size() > brace + 1)
        {
            extra = name.substr(brace + 1, name.size() - brace - 2);
        }
        for (std::size_t i = 0; i < bounds_.size(); ++i)
        {
            out << "mxl_replay_" << base << "_bucket{le=\"" << bounds_[i] << '"';
            if (!extra.empty())
            {
                out << ',' << extra;
            }
            out << "} " << hist.buckets[i] << '\n';
        }
        out << "mxl_replay_" << base << "_sum";
        if (!extra.empty())
        {
            out << '{' << extra << '}';
        }
        out << ' ' << hist.sum << '\n';
        out << "mxl_replay_" << base << "_count";
        if (!extra.empty())
        {
            out << '{' << extra << '}';
        }
        out << ' ' << hist.count << '\n';
    }
    return out.str();
}
} // namespace replay
