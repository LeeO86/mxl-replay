#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace replay
{
using Labels = std::vector<std::pair<std::string, std::string>>;

class Metrics
{
public:
    void inc(std::string const& name, Labels const& labels = {}, double value = 1);
    void set(std::string const& name, Labels const& labels, double value);
    void observe(std::string const& name, Labels const& labels, double seconds);
    [[nodiscard]] std::string render() const;

private:
    struct Series
    {
        double value = 0;
        bool gauge = false;
    };
    struct Hist
    {
        std::uint64_t count = 0;
        double sum = 0;
        std::vector<std::uint64_t> buckets;
    };
    static std::string key(std::string const& name, Labels const& labels);
    static std::string labelText(Labels labels);

    mutable std::mutex mutex_;
    std::map<std::string, Series> series_;
    std::map<std::string, Hist> histograms_;
    std::vector<double> bounds_{0.001, 0.002, 0.005, 0.008, 0.012, 0.02, 0.04};
};
} // namespace replay
