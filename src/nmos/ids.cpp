#include "nmos/ids.hpp"

#include "util/uuid.hpp"

namespace replay
{
namespace
{
std::string nameFor(std::string const& seed, std::string const& tail)
{
    return uuidV5(kUuidNamespaceUrl, "mxl-replay/" + seed + "/" + tail);
}
} // namespace

std::string NmosIds::videoReceiver(int camera, int phase) const
{
    return nameFor(seed, "in/" + std::to_string(camera) + "/video/" + std::to_string(phase));
}

std::string NmosIds::audioReceiver(int camera) const
{
    return nameFor(seed, "in/" + std::to_string(camera) + "/audio");
}

std::string NmosIds::videoSource(int channel) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/video/source");
}

std::string NmosIds::videoSender(int channel) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/video/sender");
}

std::string NmosIds::videoFlow(int channel, std::string const& formatToken) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/video/flow/" + formatToken);
}

std::string NmosIds::audioSource(int channel) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/audio/source");
}

std::string NmosIds::audioSender(int channel) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/audio/sender");
}

std::string NmosIds::audioFlow(int channel) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/audio/flow");
}

std::string NmosIds::dataSource(int channel) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/data/source");
}

std::string NmosIds::dataSender(int channel) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/data/sender");
}

std::string NmosIds::dataFlow(int channel, std::string const& formatToken) const
{
    return nameFor(seed, "out/" + std::to_string(channel) + "/data/flow/" + formatToken);
}

NmosIds makeNmosIds(std::string const& seed)
{
    NmosIds ids;
    ids.seed = seed;
    ids.node = nameFor(seed, "node");
    ids.device = nameFor(seed, "device");
    ids.domain = nameFor(seed, "domain");
    return ids;
}
} // namespace replay
