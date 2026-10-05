#pragma once

#include "config/config.hpp"
#include "flow/dis.hpp"
#include "library/catalog.hpp"
#include "media/frame.hpp"
#include "nmos/ids.hpp"
#include "ops/metrics.hpp"
#include "playout/scheduler.hpp"
#include "playout/shotbox.hpp"
#include "record/ring.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace replay
{
struct RenderedFrame
{
    std::vector<std::uint8_t> v210;
    std::vector<float> audio;
    std::vector<std::uint8_t> anc;
    std::string timecode;
    std::string motion;
    double speed = 1;
    std::uint64_t positionNs = 0;
    bool black = false;
    bool exact = false;
    int camera = 1;
};

struct Route
{
    bool active = false;
    std::string domainId;
    std::string flowId;
    std::string senderId;
    std::string state = "waiting";
};

// Clips of uploaded files play from the library ring, not from a camera.
constexpr int kLibraryCamera = 0;

class Engine
{
public:
    // Checks the storage and opens the catalog. The buffer is opened by openBuffer().
    explicit Engine(Config config, std::map<std::string, std::string> settings = {});
    ~Engine();

    // Indexes the retained segments (minutes for hours of buffer) and protects the
    // clips again. Call once, before recording, playout or the API use the engine.
    void openBuffer();
    [[nodiscard]] bool bufferReady() const { return bufferReady_; }

    [[nodiscard]] Config const& config() const { return config_; }
    [[nodiscard]] Metrics& metrics() { return metrics_; }
    [[nodiscard]] NmosIds const& ids() const { return ids_; }

    void ingestVideo(int camera, int phase, std::uint64_t taiNs, Frame10 frame);
    // Packed v210 from an MXL grain. One device upload when nvJPEG is active.
    void ingestV210(int camera, int phase, std::uint64_t taiNs, std::uint8_t const* packed, std::size_t bytes);
    // Grains the recorder could not read in time (they left the input ring).
    void countDropped(int camera, std::uint64_t grains);
    void ingestAudio(int camera, std::uint64_t taiNs, std::vector<float> audio, int channels);

    [[nodiscard]] RenderedFrame render(int channel, std::uint64_t outputTaiNs);

    void play(int channel);
    void pause(int channel);
    void live(int channel);
    void setSpeed(int channel, double speed);
    void scrubFrames(int channel, int frames);
    void scrubSeconds(int channel, double seconds);
    void setPosition(int channel, std::uint64_t taiNs);
    void markIn(int channel);
    void markOut(int channel);
    void gotoIn(int channel);
    void gotoOut(int channel);
    void setAngle(int channel, int camera);
    void setMotion(int channel, MotionMode mode);
    void setAudioMode(int channel, AudioMode mode);
    void setTimecodeMode(int channel, TcMode mode);
    void setLock(int channel, bool enabled);

    // Empty id and a message in error when protection would exceed the cap.
    std::string createClip(int channel, std::string const& name, bool allAngles, bool force, std::string& error);
    void updateClip(ClipRef clip);
    void deleteClip(std::string const& id);
    [[nodiscard]] std::vector<ClipRef> clips() const;
    ShotState shotClick(int channel, std::string const& clipId);
    void playClip(int channel, std::string const& clipId);

    std::string createPlaylist(std::string const& name, std::vector<PlaylistEntry> const& entries);
    void deletePlaylist(std::string const& id);
    [[nodiscard]] std::vector<Catalog::Playlist> playlists() const;
    void playPlaylist(int channel, std::string const& id);

    std::string upload(std::uint8_t const* data, std::size_t size, std::string const& name, std::string& error);
    std::string exportClip(std::string const& id, std::string& error);
    bool consolidate(std::string const& id, std::string& error);

    void setRoute(int camera, int phase, bool video, Route route, bool persist = false);
    [[nodiscard]] Route route(int camera, int phase, bool video) const;

    [[nodiscard]] std::string exportConfigJson() const;
    struct ImportResult
    {
        bool ok = false;
        bool restartRequired = false;
        std::string error;
    };
    ImportResult importConfigJson(std::string const& body);

    [[nodiscard]] std::string statusJson() const;
    [[nodiscard]] std::vector<std::uint8_t> previewJpeg(int channel) const;
    [[nodiscard]] bool gpuInterpolate() const;
    void setGpuPresent(bool present);

    [[nodiscard]] double storageBytesPerSecond() const { return storageBps_; }
    [[nodiscard]] std::uint64_t freeBytes() const;
    // Sets the per-camera recorder and storage metrics (called by /metrics).
    void updateMetrics();

private:
    struct PhaseSlot
    {
        bool present = false;
        Frame10 frame;
        std::uint64_t taiNs = 0;
    };
    struct CameraRuntime
    {
        FrameRing ring;
        std::vector<PhaseSlot> phases;
        std::uint64_t openHouse = 0;
        bool houseOpen = false;
        std::vector<float> audio;
        int audioChannels = 2;
        std::uint64_t recorded = 0;
        std::uint64_t dropped = 0;
        std::uint64_t phaseMissing = 0;
        bool scaled = false;
        std::vector<std::uint8_t> preview;
        CameraRuntime(std::string directory, std::uint64_t retentionNs, int segmentSeconds);
    };
    struct ChannelRuntime
    {
        Scheduler scheduler;
        Shotbox shot;
        bool liveMode = true;
        bool black = false;
        bool hasIn = false;
        bool hasOut = false;
        std::uint64_t inNs = 0;
        std::uint64_t outNs = 0;
        std::string clipId;
        std::string playlistId;
        int playlistIndex = -1;
        int fadeFramesLeft = 0;
        Frame10 last;
        std::vector<std::uint8_t> lastV210;
        std::vector<std::uint8_t> preview;
        std::uint64_t late = 0;
        std::uint64_t ancSequence = 0;
    };

    // Encode into `pending`; pushFrames() writes them after the engine lock is released.
    void flushHouse(CameraRuntime& camera, CameraConfig const& cfg, std::vector<StoredFrame>& pending);
    void storeFrame(CameraRuntime& camera, std::uint64_t taiNs, Frame10 const& frame, std::vector<float> const& audio, std::vector<StoredFrame>& pending);
    void pushFrames(CameraRuntime& camera, std::vector<StoredFrame> frames);
    // The ring a clip or channel camera plays from (kLibraryCamera: uploads); null when unknown.
    [[nodiscard]] FrameRing const* ringOf(int camera) const;
    [[nodiscard]] FrameRing* ringOf(int camera);
    void removeStaleCameras();
    void continueSerial();
    Frame10 frameAt(int camera, std::uint64_t taiNs, bool* found) const;
    void finishClip(int channel);
    [[nodiscard]] std::uint64_t sourcePeriod(int camera) const;
    [[nodiscard]] double hfrFactor(int camera) const;
    void loadRoutes();
    void saveRoutes() const;
    [[nodiscard]] std::string routesPath() const;

    Config config_;
    std::map<std::string, std::string> settings_;
    NmosIds ids_;
    Metrics metrics_;
    Catalog catalog_;
    mutable std::recursive_mutex mutex_;
    std::vector<CameraRuntime> cameras_;
    std::unique_ptr<FrameRing> library_;
    // One upload at a time: each starts after the library's newest frame.
    std::mutex uploadMutex_;
    std::atomic<bool> bufferReady_{false};
    std::vector<ChannelRuntime> channels_;
    std::map<std::string, Route> routes_;
    std::map<std::string, FlowPair> flowCache_;
    bool gpu_ = false;
    double storageBps_ = 0;
    std::uint64_t clipSerial_ = 1;
    std::string libraryDir_;
};
} // namespace replay
