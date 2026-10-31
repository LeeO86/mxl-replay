#pragma once

#include "config/config.hpp"
#include "flow/dis.hpp"
#include "library/catalog.hpp"
#include "media/convert.hpp"
#include "media/frame.hpp"
#include "media/mosaic.hpp"
#include "nmos/ids.hpp"
#include "ops/metrics.hpp"
#include "ops/preview.hpp"
#include "playout/decodeahead.hpp"
#include "playout/flowahead.hpp"
#include "playout/readahead.hpp"
#include "playout/scheduler.hpp"
#include "playout/shotbox.hpp"
#include "playout/transition.hpp"
#include "media/audio.hpp"
#include "record/ring.hpp"
#include "tally/tsl.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace replay
{
struct GpuPictures;
class AheadTaker;
struct RenderedFrame
{
    std::vector<std::uint8_t> v210;
    std::vector<float> audio;
    std::vector<std::uint8_t> anc;
    // Interlaced house: the second field's ANC grain (`anc` is the first field's).
    std::vector<std::uint8_t> ancSecond;
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

// Audio-only library items (1.6.0) have ids "audio-N"; their PCM is <library>/audio/<id>.f32.
constexpr char const* kAudioItemPrefix = "audio-";

// Clips and playlist entries play forward at 1 %–200 %: at 0 % or backwards they never reach OUT.
constexpr double kMinClipSpeed = 0.01;
constexpr double kMaxClipSpeed = 2.0;

// The result of a clip edit (updateClip).
enum class ClipEdit
{
    Ok,
    Unknown,      // no such clip
    NoFrame,      // a new IN or OUT has no recorded frame within one frame period
    OutBeforeIn,  // after landing on frames, OUT is before IN
    OverCap,      // the range grows while clips keep REPLAY_PROTECT_MAX_PCT of the storage
};

class Engine
{
public:
    // Checks the storage and opens the catalog. The buffer is opened by openBuffer().
    // `settings`: every key that is set (the export, the import's base); `state`: the keys the UI owns,
    // which saveSettings writes to the state directory's config.json (1.8.2: not the platform's keys).
    explicit Engine(Config config, std::map<std::string, std::string> settings = {}, std::map<std::string, std::string> state = {});
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
    // Plans every channel's flows again (render does it each grain): after a command, so the flows its
    // next grains need start now (SPECIFICATION.md §5.3).
    void planFlows();

    void play(int channel);
    void pause(int channel);
    void live(int channel);
    // Stop (SPECIFICATION.md §7): the channel's idle playlist plays (a paused one resumes); without one the shot
    // ends where it stands (Ended, paused). A live channel without an idle playlist stays live.
    void stop(int channel);
    // CH<n>_IDLE_PLAYLIST, kept in the state directory's config.json; empty: none. A playlist set plays at once,
    // looped, on an ended channel; a busy one plays it at its next clip end, stop or playlist end. False with `error` "unknown playlist" or when it cannot be written.
    bool setIdlePlaylist(int channel, std::string const& id, std::string& error);
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
    // CH<n>_PLAY_ON_TALLY, kept in the state directory's config.json. False with `error` when it cannot be written.
    bool setPlayOnTally(int channel, bool enabled, std::string& error);
    // A TSL UMD 5.0 message (SPECIFICATION.md §8.8): the displays of TSL_SCREEN set the tally of the channels TSL_MAP
    // gives them. A rising red (LH or the text tally) on a channel with CH<n>_PLAY_ON_TALLY plays its Cued or Paused
    // shot, as POST /api/v1/control {"action":"play"} does. Counts the packet and its errors.
    void applyTally(TslMessage const& message);

    // Empty id and a message in error when protection would exceed the cap. An empty colour
    // keeps the camera's colour.
    std::string createClip(int channel, std::string const& name, bool allAngles, bool force, std::string& error, std::string const& colour = {},
        std::string const& tags = {});
    // Stores the clip's fields. A new IN or OUT lands on the nearest recorded frame and the
    // protected range moves with it; growing the range at the protection cap needs `force`.
    ClipEdit updateClip(ClipRef clip, bool force = false);
    // False when the id is unknown.
    bool deleteClip(std::string const& id);
    [[nodiscard]] std::vector<ClipRef> clips() const;
    // Empty id when unknown.
    [[nodiscard]] ClipRef clip(std::string const& id) const;
    // A shotbox button: a clip id or a playlist id. One click cues, the next plays, the next
    // pauses, the next resumes (REPLAY_PLAY_ON_FIRST_CLICK: play, pause, resume).
    ShotState shotClick(int channel, std::string const& id);
    void playClip(int channel, std::string const& clipId);

    std::string createPlaylist(std::string const& name, std::vector<PlaylistEntry> const& entries, bool loop = false, AuxTrack const& aux = {});
    // An aux source: "" (none), a clip or an audio item.
    [[nodiscard]] bool auxSourceExists(std::string const& source) const;
    [[nodiscard]] std::vector<Catalog::AudioItem> audioItems() const;
    // Empty id when unknown.
    [[nodiscard]] Catalog::AudioItem audioItem(std::string const& id) const;
    bool renameAudioItem(std::string const& id, std::string const& name);
    // Removes the item and its file, and the aux of the playlists that use it. False when unknown.
    bool deleteAudioItem(std::string const& id);
    // False when the id is unknown. A channel playing the playlist stays on its entry.
    bool updatePlaylist(Catalog::Playlist const& playlist);
    bool deletePlaylist(std::string const& id);
    [[nodiscard]] std::vector<Catalog::Playlist> playlists() const;
    // Empty id when unknown.
    [[nodiscard]] Catalog::Playlist playlist(std::string const& id) const;
    // Plays the playlist from its first clip that exists; `play` false cues it there (paused, shot Cued).
    void playPlaylist(int channel, std::string const& id, bool play = true);

    // A clip ("upload-N"), or an audio item ("audio-N") for a file with audio and no video.
    std::string upload(std::uint8_t const* data, std::size_t size, std::string const& name, std::string& error);
    // The same for a file on disk (an HTTP upload's body, 1.8.2): read and stored one frame at a time.
    std::string uploadFile(std::string const& path, std::string const& name, std::string& error);
    // Where the HTTP server writes upload bodies (in the storage directory; emptied at start).
    [[nodiscard]] std::string uploadDirectory() const { return config_.storageDir + "/incoming"; }
    // Whether an upload body of `bytes` may come in: {0, ""}, or {413, reason} over REPLAY_UPLOAD_MAX_MB,
    // or {507, reason} when the disk cannot take the body, about kUploadExpansion times it in frames,
    // the camera buffers' remaining growth and kUploadReserveBytes.
    [[nodiscard]] std::pair<int, std::string> admitUpload(std::uint64_t bytes) const;
    static constexpr std::uint64_t kUploadExpansion = 8;
    static constexpr std::uint64_t kUploadReserveBytes = 1ull << 30;
    // Stops a running upload (shutdown): it fails with "shutting down" after its current frame.
    void stopUploads() { uploadsStopped_.store(true); }
    std::string exportClip(std::string const& id, std::string& error);
    bool consolidate(std::string const& id, std::string& error);

    void setRoute(int camera, int phase, bool video, Route route, bool persist = false);
    // The recorder's status of a route ("waiting", "running", ...): set only while the route is still the one it
    // read (active, same domain, flow and sender), so an IS-05 activation in between is never overwritten.
    // False when the route changed.
    bool setRouteState(int camera, int phase, bool video, Route const& expected, std::string const& state);
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
    // UI previews, made on request (nothing is spent while no UI asks) and kept a little while
    // for other UIs: the channel's last output grain, 640 pixels wide (150 ms); the camera's
    // newest recorded frame (1 s) and a clip's IN frame (until IN changes), decoded at 1/2–1/8
    // scale. Empty when there is no such channel, camera, clip or frame.
    [[nodiscard]] std::vector<std::uint8_t> previewJpeg(int channel) const;
    [[nodiscard]] std::vector<std::uint8_t> cameraPreviewJpeg(int camera) const;
    [[nodiscard]] std::vector<std::uint8_t> clipThumbnailJpeg(std::string const& id) const;
    [[nodiscard]] bool gpuInterpolate() const;
    void setGpuPresent(bool present);

    // The WebRTC preview (REPLAY_PREVIEW_MODE=webrtc, SPECIFICATION.md §8.6): every channel and camera
    // (phase 1) draws its tile into `mosaic` on every mosaicStep-th grain. Set before recording and
    // playout start; null (the default) draws nothing. `status` gives the publish state for the status
    // and the metrics.
    void setPreviewMosaic(PreviewMosaic* mosaic);
    void setPreviewStatus(std::function<PreviewStatus()> status);

    // The write throughput measured at start.
    [[nodiscard]] double storageBytesPerSecond() const { return storageBps_; }
    [[nodiscard]] std::uint64_t freeBytes() const;
    // Sets the per-camera recorder and storage metrics (called by /metrics). The recorder's write
    // rate is taken over the time since the last call, at least a second.
    void updateMetrics(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // How far ahead the stored frames a channel plays are read (SPECIFICATION.md §5.3): kReadAheadNs of frames
    // ahead of its position (times the speed above 1), and the first kReadAheadNs of the clip it plays after OUT
    // once OUT is kReadAheadLeadNs (plus the playlist's longest transition) away.
    static constexpr std::uint64_t kReadAheadNs = 1'000'000'000;
    static constexpr std::uint64_t kReadAheadLeadNs = 2'000'000'000;
    // Called on the loader thread after each read-ahead with the request and the bytes read (tests).
    void setReadAheadObserver(std::function<void(ReadAhead::Request const&, std::uint64_t)> observer);

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
        CameraRuntime(std::string directory, std::uint64_t retentionNs, int segmentSeconds);
    };
    // A stored picture the output decoded (1.8.3). An upload at a lower frame rate stores each source frame once
    // per house frame (1.8.1): the same JPEG on the next grain is not decoded again. `key`: the GPU decoder's key
    // for it; `v210`: the CPU decode (empty when the GPU decoded it).
    struct DecodedPicture
    {
        std::vector<std::uint8_t> jpeg;
        std::string key;
        std::vector<std::uint8_t> v210;
    };
    // The slots of a channel's DecodeAhead: on the device with a GPU, else v210 grains.
    struct AheadPictures
    {
        std::shared_ptr<GpuPictures> gpu;
        std::array<std::vector<std::uint8_t>, DecodeAhead::kSlots> v210;
        // The JPEG the worker read last (the worker only): a repeat of it is not decoded.
        std::vector<std::uint8_t> lastJpeg;
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
        // The audio fade: 0 audible, 2 silent; each grain moves one step (render).
        int fadeFramesLeft = 0;
        // The house time of the audio the last grain read (0: none yet).
        std::uint64_t audioNs = 0;
        Frame10 last;
        std::vector<std::uint8_t> lastV210;
        // The stored picture the last grain decoded (this channel's playout thread only).
        DecodedPicture decoded;
        std::string timecode; // of the last output grain (its ANC)
        std::uint64_t late = 0;
        // The stored pictures of the next grains, decoded ahead on a worker (SPECIFICATION.md §5.1, 1.8.5).
        std::unique_ptr<DecodeAhead> decodeAhead;
        std::shared_ptr<AheadPictures> pictures;
        // The flows of the next grains (SPECIFICATION.md §5.3) and the grains blended without one.
        std::unique_ptr<FlowAhead> ahead;
        LogLimit fallbackLog;
        // The last TSL tally (0 off, 1 red, 2 green, 3 amber) and when red last played the channel (TAI ns).
        int tslLh = 0;
        int tslRh = 0;
        int tslText = 0;
        std::uint64_t tallyPlayNs = 0;
        // The channel plays its idle playlist (looped, every entry plays on).
        bool idle = false;
        // The current entry's own audio plays (a playlist entry's `own_audio`; true otherwise).
        bool ownAudio = true;
        // A transition into the current entry (SPECIFICATION.md §7, 1.6.0): the outgoing entry's side.
        struct Mix
        {
            bool active = false;
            Scheduler scheduler; // the outgoing entry, stepping on to its OUT
            std::uint64_t outNs = 0;
            int videoFrames = 0;
            int audioFrames = 0;
            int done = 0; // grains of the transition rendered
            std::int64_t audioSamples = 0; // crossfade samples done
            std::int64_t audioTotal = 0;
            bool ownAudio = true;
            AudioMode audio = AudioMode::Mute;
            std::uint64_t audioNs = 0; // the house time of the outgoing audio read last
        } mix;
        // The playlist the channel's state was read for (playlistId and the catalog version), and what it gave.
        std::string planKey;
        Catalog::Playlist plan;
        // Its entries' playing times (-1: a deleted clip), the longest transition in it, and the aux source's length.
        std::vector<std::int64_t> planLengths;
        int planMaxTransition = 0;
        std::int64_t planAuxLength = 0;
        // The aux track (SPECIFICATION.md §5.4, 1.6.0): the playlist it plays for, its sample position, the run
        // gain (0 paused … 1 playing) and the duck. `auxRestart` starts it from 0 on the next grain.
        std::string auxPlaylist;
        AuxTrack aux;
        std::int64_t auxPosition = 0;
        double auxRun = 0;
        DuckRamp duck;
        bool auxRestart = false;
        double auxGainDb = -120;
        // The frames read ahead of the position (camera -1: none), and what the clip after OUT was read for.
        int readAheadCamera = -1;
        std::uint64_t readAheadFrom = 0;
        std::uint64_t readAheadTo = 0;
        std::string readAheadNext;
    };
    // What finishClip does at OUT (planClipEnd; lock held).
    struct ClipEnd
    {
        EndAction action = EndAction::Freeze; // a looped playlist's wrap is Next
        bool idleNext = false;                // the idle playlist plays its entry `next`
        int next = -1;                        // the playlist entry played or cued next; -1: none
        ClipRef nextClip;
        bool autoAdvance = false;
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
    // The stored frame nearest `taiNs` decoded straight into `v210` (house format). `found` says
    // whether a frame exists; false with found set means it is not in the house format.
    // A picture the channel's worker decoded ahead is copied from there (`ahead`, may be null).
    bool v210At(int camera, std::uint64_t taiNs, std::vector<std::uint8_t>& v210, bool* found, DecodedPicture& last, bool* repeated,
        AheadTaker* ahead = nullptr) const;
    void finishClip(int channel);
    // What finishClip will do at OUT with the channel on `list` (lock held).
    [[nodiscard]] ClipEnd planClipEnd(ChannelRuntime const& runtime, Catalog::Playlist const& list) const;
    // Posts the stored frames the channel needs soon to the loader thread (lock held, no reads).
    void readAhead(int channel);
    // The shotbox countdowns of a channel on `list` (SPECIFICATION.md §8.1): the time to the playlist's end, and to
    // OUT in `clipLeft`; -1 for none (lock held).
    std::int64_t countdowns(ChannelRuntime const& channel, Catalog::Playlist const& list, std::int64_t& clipLeft) const;
    // The same with the entries' playing times given (-1: a deleted clip).
    std::int64_t countdowns(ChannelRuntime const& channel, Catalog::Playlist const& list, std::vector<std::int64_t> const& lengths, std::int64_t& clipLeft) const;
    // The channel's playlist, read again only when it or the catalog changed (lock held).
    Catalog::Playlist const& currentPlaylist(int channel);
    // Starts a mix or crossfade into the next entry when the current one has the transition's length left and
    // finishClip would play the next entry (lock held). The channel moves to the next entry at once.
    void maybeStartTransition(int channel);
    // The aux track mixed into one grain's `pcm` (lock held, released for the reads).
    void mixAux(int channel, std::unique_lock<std::recursive_mutex>& lock, std::vector<float>& pcm, bool still, bool ownAudible, std::uint64_t outputTaiNs);
    // `count` stereo sample frames of an aux source from `position` (zeros past its end); its length in sample frames.
    std::vector<float> readAuxSamples(std::string const& source, ClipRef const& clip, std::int64_t position, std::int64_t count) const;
    [[nodiscard]] std::int64_t auxLength(std::string const& source) const;
    [[nodiscard]] std::string audioItemPath(std::string const& id) const;
    // Plays the channel's idle playlist from its first entry (lock held). False when it has none or no clip of it exists.
    bool startIdle(int channel);
    // Posts the pairs the channel's next grains interpolate to its flow worker. Lock held.
    void planFlows(int channel);
    // Posts the stored frames the channel's next grains show, and the current grain (`current`) does not, to its
    // decode worker. Lock held.
    void planDecodes(int channel, SourcePick const& current, MotionMode motion);
    // Decodes a planned frame into slot `slot` of `pictures` on the channel's decode worker (no engine lock).
    bool decodeAheadFrame(AheadPictures& pictures, AheadFrame const& frame, int slot, std::uint64_t& storedNs) const;
    // Counts a grain's lookups of pictures decoded ahead.
    void countDecodeAhead(int channel, AheadTaker const& taker);
    // A pair's flow for the channel's worker: on the GPU when there is one (its GPU time is observed), else on the CPU.
    AheadFlow computeFlow(FlowAhead::Pair const& pair, int channel);
    // Back to live on the channel (lock held). Uploads have no live picture: live follows camera 1.
    void goLive(ChannelRuntime& runtime);
    // Clips keep REPLAY_PROTECT_MAX_PCT of the storage budget or more (lock held).
    [[nodiscard]] bool protectionFull() const;
    // A clip that holds a protected range: not one consolidated out of a camera buffer.
    [[nodiscard]] static bool holdsRange(ClipRef const& clip) { return !(clip.library && clip.camera != kLibraryCamera); }
    // Puts the channel on the clip's IN (camera, marks, speed, modes); `entry` is the playlist
    // entry being played, which brings its own speed and end action. Lock held.
    void cueClip(int channel, ClipRef const& clip, PlaylistEntry const* entry);
    // The stored frame nearest taiNs as a small JPEG (decodeJpegPreview); empty when there is none.
    [[nodiscard]] std::vector<std::uint8_t> storedPreview(int camera, std::uint64_t taiNs) const;
    // A preview made less than `maxAge` ago (zero: any age), or empty.
    [[nodiscard]] std::vector<std::uint8_t> recentPreview(std::string const& key, std::chrono::milliseconds maxAge) const;
    void keepPreview(std::string const& key, std::vector<std::uint8_t> const& jpeg) const;
    [[nodiscard]] std::uint64_t sourcePeriod(int camera) const;
    [[nodiscard]] double hfrFactor(int camera) const;
    // Whether the mosaic is drawn for the house grain at `taiNs`.
    [[nodiscard]] bool mosaicDue(std::uint64_t taiNs) const;
    void loadRoutes();
    void saveRoutes() const;
    // Writes settings_ to the state directory's config.json (lock held). False with `error` when it cannot.
    bool saveSettings(std::string& error) const;
    [[nodiscard]] std::string routesPath() const;

    Config config_;
    // The keys the UI set (saved), and every key that is set (exported).
    std::map<std::string, std::string> settings_;
    std::map<std::string, std::string> effective_;
    NmosIds ids_;
    Metrics metrics_;
    Catalog catalog_;
    mutable std::recursive_mutex mutex_;
    std::vector<CameraRuntime> cameras_;
    std::unique_ptr<FrameRing> library_;
    // One black grain, made once (idle and black channels).
    std::vector<std::uint8_t> blackV210_;
    // One upload at a time: each starts after the library's newest frame.
    std::mutex uploadMutex_;
    std::atomic<bool> uploadsStopped_{false};
    std::uint64_t cameraBudgetBytes_ = 0; // the camera buffers' size on disk (estimate at start)
    std::string storeUpload(UploadSource const& source, std::string const& name, std::string& error);
    struct Preview
    {
        std::chrono::steady_clock::time_point made;
        std::vector<std::uint8_t> jpeg;
    };
    mutable std::mutex previewMutex_;
    mutable std::map<std::string, Preview> previews_;
    std::atomic<bool> bufferReady_{false};
    std::vector<ChannelRuntime> channels_;
    std::map<std::string, Route> routes_;
    bool gpu_ = false;
    double storageBps_ = 0;
    // Bytes the recorder wrote, and the write rate made from them by updateMetrics.
    std::uint64_t writtenBytes_ = 0;
    std::uint64_t rateBytes_ = 0;
    std::chrono::steady_clock::time_point rateAt_ = std::chrono::steady_clock::now();
    double writeBps_ = 0;
    // TAI at construction: a camera's last-frame age counts from here at most.
    std::uint64_t startedNs_ = 0;
    std::uint64_t clipSerial_ = 1;
    // Bumped by every catalog change a channel's cached playlist depends on.
    std::uint64_t catalogVersion_ = 0;
    std::string libraryDir_;
    PreviewMosaic* mosaic_ = nullptr;
    std::uint64_t mosaicStep_ = 1;
    std::function<PreviewStatus()> previewStatus_;
    std::mutex readAheadMutex_;
    std::function<void(ReadAhead::Request const&, std::uint64_t)> readAheadObserver_;
    // Last: its thread reads the rings and ends before they are destroyed.
    std::unique_ptr<ReadAhead> readAhead_;
};
} // namespace replay
