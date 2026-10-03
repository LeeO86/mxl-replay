#pragma once

#include "flow/dis.hpp"
#include "media/audio.hpp"
#include "media/format.hpp"
#include "playout/scheduler.hpp"
#include "playout/shotbox.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace replay
{
class ConfigError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

enum class IdleSource
{
    Last,
    Black,
    E2e
};
enum class TcMode
{
    Source,
    Output
};
enum class FlowModuleKind
{
    DisCuda,
    Ofa
};

struct CameraConfig
{
    int index = 1;
    std::string label;
    std::string colour = "#3b82f6";
    bool record = true;
    bool audio = true;
    int phases = 1;
    double bufferHours = 2;
    int nativeFps = 0;
};

struct ChannelConfig
{
    int index = 1;
    std::string label;
    IdleSource idle = IdleSource::Last;
    MotionMode motion = MotionMode::Interpolate;
    AudioMode audio = AudioMode::Mute;
    TcMode timecode = TcMode::Source;
    FlowModuleKind flow = FlowModuleKind::DisCuda;
    int atmosCamera = 0;
    bool lockToFirst = false;
};

struct Config
{
    std::string hostId;
    VideoFormat format;
    int inputs = 4;
    int channels = 2;
    std::string storageDir = "/data/replay";
    double bufferHours = 2;
    int jpegQuality = 92;
    int protectMaxPct = 50;
    FlowModuleKind flowModule = FlowModuleKind::DisCuda;
    InterpPreset preset = InterpPreset::Balanced;
    AudioMode slowmoAudio = AudioMode::Mute;
    int rampFrames = 3;
    double hfrSnap = 0.1;
    int segmentSeconds = 10;
    int liveDelayFrames = 2;
    bool playOnFirstClick = false;
    bool allowCpuInterp = false;
    double storageMinMbps = 100;
    bool odirect = false;
    bool synthetic = false;
    std::string scanPath = "/Volumes/mxl";
    std::string outputDomainDir;
    std::string outputDomainId;
    bool nmosEnable = true;
    std::string nmosRegistryAddress;
    int nmosRegistryPort = 3210;
    bool nmosDnsSd = false;
    int nmosPort = 3302;
    std::string nmosSeed;
    bool webEnable = true;
    int webPort = 8150;
    std::string logLevel = "info";
    std::string logFormat = "json";
    std::string configFile;
    std::vector<CameraConfig> cameras;
    std::vector<ChannelConfig> channelList;
};

struct LoadedConfig
{
    Config config;
    std::map<std::string, std::string> flat;
};

[[nodiscard]] std::vector<std::string> configKeys();
[[nodiscard]] LoadedConfig loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file);
[[nodiscard]] std::map<std::string, std::string> readConfigFile(std::string const& path);
[[nodiscard]] std::string exportKeyValue(Config const& config);
[[nodiscard]] std::string hostnameString();

[[nodiscard]] char const* idleName(IdleSource idle);
[[nodiscard]] char const* tcModeName(TcMode mode);
[[nodiscard]] char const* flowModuleName(FlowModuleKind kind);
} // namespace replay
