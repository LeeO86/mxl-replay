#pragma once

#include <string>

namespace replay
{
struct NmosIds
{
    std::string seed;
    std::string node;
    std::string device;
    std::string domain;
    [[nodiscard]] std::string videoReceiver(int camera, int phase) const;
    [[nodiscard]] std::string audioReceiver(int camera) const;
    [[nodiscard]] std::string videoSource(int channel) const;
    [[nodiscard]] std::string videoSender(int channel) const;
    [[nodiscard]] std::string videoFlow(int channel, std::string const& formatToken) const;
    [[nodiscard]] std::string audioSource(int channel) const;
    [[nodiscard]] std::string audioSender(int channel) const;
    [[nodiscard]] std::string audioFlow(int channel) const;
    [[nodiscard]] std::string dataSource(int channel) const;
    [[nodiscard]] std::string dataSender(int channel) const;
    [[nodiscard]] std::string dataFlow(int channel, std::string const& formatToken) const;
};

[[nodiscard]] NmosIds makeNmosIds(std::string const& seed);
} // namespace replay
