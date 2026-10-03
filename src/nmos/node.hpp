#pragma once

#include "app/engine.hpp"

#include <memory>
#include <string>

namespace replay
{
class NmosNode
{
public:
    NmosNode(Config config, Engine& engine);
    ~NmosNode();
    void start();
    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::string summary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace replay
