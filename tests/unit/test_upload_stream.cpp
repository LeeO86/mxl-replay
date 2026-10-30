// Uploads are streamed (1.8.2): the HTTP body goes to a file, and the file is converted one house frame
// at a time with the audio read by its own demuxer.
#include "app/engine.hpp"
#include "config/config.hpp"
#include "media/convert.hpp"
#include "media/format.hpp"
#include "media/jpeg.hpp"
#include "media/timebase.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "record/ring.hpp"

#include <doctest/doctest.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <limits>
#include <iostream>
#include <sstream>
#include <thread>

using namespace replay;

namespace
{
constexpr int kW = 16;
constexpr int kH = 8;

// Sample k of the test tone: a ramp, so a slice shows where it was cut.
float rampSample(std::int64_t k)
{
    return static_cast<float>(k % 1000) / 1000.0F;
}

// A NUT file with `frames` 16×8 rawvideo frames at 25p (frame i luma 16 + 4·i) and 48 kHz stereo
// float PCM of the same length (left: the ramp, right: −ramp). `audioLast`: every video packet before the
// first audio packet (the worst interleave), else in time order.
void writeAv(std::filesystem::path const& path, int frames, bool audioLast)
{
    AVFormatContext* fmt = nullptr;
    REQUIRE(avformat_alloc_output_context2(&fmt, nullptr, "nut", path.c_str()) >= 0);
    AVStream* vs = avformat_new_stream(fmt, nullptr);
    vs->time_base = AVRational{1, 1000};
    vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    vs->codecpar->format = AV_PIX_FMT_YUV420P;
    vs->codecpar->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_YUV420P);
    vs->codecpar->width = kW;
    vs->codecpar->height = kH;
    vs->avg_frame_rate = AVRational{25, 1};
    AVStream* as = avformat_new_stream(fmt, nullptr);
    as->time_base = AVRational{1, 1000};
    as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    as->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
    as->codecpar->sample_rate = 48000;
    as->codecpar->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    as->codecpar->format = AV_SAMPLE_FMT_FLT;
    as->codecpar->bits_per_coded_sample = 32;
    as->codecpar->block_align = 8;
    REQUIRE(avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(fmt, nullptr) >= 0);
    auto video = [&](int i) {
        std::vector<std::uint8_t> picture(static_cast<std::size_t>(kW * kH), static_cast<std::uint8_t>(16 + 4 * i));
        picture.insert(picture.end(), static_cast<std::size_t>(kW * kH / 2), 128);
        AVPacket* packet = av_packet_alloc();
        av_new_packet(packet, static_cast<int>(picture.size()));
        std::memcpy(packet->data, picture.data(), picture.size());
        packet->stream_index = vs->index;
        packet->pts = packet->dts = av_rescale_q(i, AVRational{1, 25}, vs->time_base);
        packet->duration = av_rescale_q(1, AVRational{1, 25}, vs->time_base);
        packet->flags |= AV_PKT_FLAG_KEY;
        REQUIRE(av_write_frame(fmt, packet) >= 0);
        av_packet_free(&packet);
    };
    // One audio packet per 40 ms (1920 samples).
    auto audio = [&](int i) {
        std::vector<float> samples;
        for (std::int64_t k = i * 1920LL; k < (i + 1) * 1920LL; ++k)
        {
            samples.push_back(rampSample(k));
            samples.push_back(-rampSample(k));
        }
        AVPacket* packet = av_packet_alloc();
        av_new_packet(packet, static_cast<int>(samples.size() * sizeof(float)));
        std::memcpy(packet->data, samples.data(), samples.size() * sizeof(float));
        packet->stream_index = as->index;
        packet->pts = packet->dts = av_rescale_q(i * 1920LL, AVRational{1, 48000}, as->time_base);
        packet->duration = av_rescale_q(1920, AVRational{1, 48000}, as->time_base);
        packet->flags |= AV_PKT_FLAG_KEY;
        REQUIRE(av_write_frame(fmt, packet) >= 0);
        av_packet_free(&packet);
    };
    for (int i = 0; i < frames; ++i)
    {
        video(i);
        if (!audioLast)
        {
            audio(i);
        }
    }
    for (int i = 0; audioLast && i < frames; ++i)
    {
        audio(i);
    }
    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
}

std::filesystem::path freshUploadDir(char const* name)
{
    auto const dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::unique_ptr<Engine> uploadEngine(std::filesystem::path const& dir)
{
    auto const loaded = loadConfig({{"REPLAY_FORMAT", "16x8p50"}, {"REPLAY_INPUTS", "1"}, {"REPLAY_CHANNELS", "1"}, {"REPLAY_BUFFER_HOURS", "0.01"},
                                       {"REPLAY_STORAGE_DIR", (dir / "media").string()}, {"REPLAY_STATE_DIR", (dir / "state").string()},
                                       {"REPLAY_STORAGE_MIN_MBPS", "0"}, {"NMOS_HOST_ADDRESS", "10.4.4.4"}, {"HOST_ID", "ci"}},
        {});
    auto engine = std::make_unique<Engine>(loaded.config, loaded.flat);
    engine->openBuffer();
    return engine;
}

// One HTTP/1.1 request on a new connection; the whole response.
std::string httpRequest(int port, std::string const& head, std::string const& body = {})
{
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return {};
    }
    auto const request = head + "\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < request.size())
    {
        auto const n = ::send(fd, request.data() + sent, std::min<std::size_t>(request.size() - sent, 100000), MSG_NOSIGNAL);
        if (n <= 0)
        {
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string response;
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
    {
        response.append(buf, buf + n);
    }
    ::close(fd);
    return response;
}

std::size_t filesIn(std::filesystem::path const& dir)
{
    std::size_t count = 0;
    for ([[maybe_unused]] auto const& entry : std::filesystem::directory_iterator(dir))
    {
        ++count;
    }
    return count;
}
} // namespace

TEST_CASE("a streamed upload gives each house frame once, in order, with its cadence of the audio, however the file interleaves it")
{
    auto const dir = freshUploadDir("mxl-replay-upload-stream");
    auto const house = parseVideoFormat("16x8p50");
    for (bool const audioLast : {false, true})
    {
        auto const path = dir / (audioLast ? "late.nut" : "even.nut");
        writeAv(path, 50, audioLast); // 2 s at 25p
        std::uint64_t next = 0;
        std::uint32_t lastSource = 0;
        std::uint32_t distinct = 0;
        std::int64_t sample = 0;
        bool contiguous = true;
        bool lumaOk = true;
        UploadSink sink;
        sink.frame = [&](std::uint64_t n, std::uint32_t source, Frame10 const& picture, std::vector<float> const& audio) {
            CHECK(n == next);
            ++next;
            if (n == 0 || source != lastSource)
            {
                ++distinct;
                lastSource = source;
            }
            // 25p in a 50p house: house frame n shows source frame n / 2 (luma 16 + 4·(n / 2), 10-bit).
            lumaOk = lumaOk && picture.y[0] == static_cast<std::uint16_t>((16 + 4 * (n / 2)) << 2);
            CHECK(audio.size() == static_cast<std::size_t>(audioSamplesForFrame(n, 50, 1, 48000)) * 2);
            for (std::size_t i = 0; i + 1 < audio.size(); i += 2)
            {
                contiguous = contiguous && std::abs(audio[i] - rampSample(sample)) < 1e-6F && std::abs(audio[i + 1] + rampSample(sample)) < 1e-6F;
                ++sample;
            }
            return true;
        };
        auto const result = streamUpload(UploadSource{path.string(), nullptr, 0}, house, sink);
        CHECK(result.error.empty());
        CHECK_FALSE(result.stopped);
        CHECK(result.frames == 100);
        CHECK(distinct == 50);
        CHECK(lumaOk);
        CHECK(contiguous);
        CHECK(sample == 96000);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a streamed upload stops when the sink says so")
{
    auto const dir = freshUploadDir("mxl-replay-upload-stop");
    writeAv(dir / "clip.nut", 50, false);
    int calls = 0;
    UploadSink sink;
    sink.frame = [&](std::uint64_t, std::uint32_t, Frame10 const&, std::vector<float> const&) { return ++calls < 10; };
    auto const result = streamUpload(UploadSource{(dir / "clip.nut").string(), nullptr, 0}, parseVideoFormat("16x8p50"), sink);
    CHECK(result.stopped);
    CHECK(calls == 10);
    CHECK(result.frames == 9);
    std::filesystem::remove_all(dir);
}

TEST_CASE("engine stores an upload from a file frame by frame, with its audio")
{
    auto const dir = freshUploadDir("mxl-replay-upload-file");
    auto engine = uploadEngine(dir);
    writeAv(dir / "clip.nut", 50, true);
    std::string error;
    auto const id = engine->uploadFile((dir / "clip.nut").string(), "file", error);
    REQUIRE_FALSE(id.empty());
    CHECK(error.empty());
    auto const clips = engine->clips();
    auto const clip = std::find_if(clips.begin(), clips.end(), [&](ClipRef const& c) { return c.id == id; });
    REQUIRE(clip != clips.end());
    CHECK(clip->outNs - clip->inNs == 99ull * 20000000ull); // 100 house frames at 50p
    CHECK(clip->name == "file");
    // A stopped upload (shutdown) stores no clip.
    engine->stopUploads();
    CHECK(engine->uploadFile((dir / "clip.nut").string(), "late", error).empty());
    CHECK(error == "shutting down");
    CHECK(engine->clips().size() == clips.size());
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("http server writes a matching body to a file and answers other requests meanwhile")
{
    auto const dir = freshUploadDir("mxl-replay-body-files");
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::string seenBody;
    std::string seenPathContent;
    std::atomic<bool> inHandler{false};
    HttpServer server;
    HttpBodyFiles files;
    files.match = [](std::string const& method, std::string const& path) { return method == "POST" && path == "/upload"; };
    files.directory = dir.string();
    files.admit = [](HttpRequest const&, std::uint64_t length) -> std::optional<HttpResponse> {
        if (length <= 3 * 1024 * 1024)
        {
            return std::nullopt;
        }
        HttpResponse refused;
        refused.status = 413;
        refused.body = "too large for the test";
        return refused;
    };
    server.start(
        0,
        [&](HttpRequest const& request) {
            HttpResponse response;
            if (request.path == "/upload")
            {
                seenBody = request.body;
                std::ifstream in(request.bodyPath, std::ios::binary);
                seenPathContent.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                inHandler.store(true);
                std::unique_lock lock{mu};
                cv.wait_for(lock, std::chrono::seconds(10), [&] { return release; });
            }
            response.body = "done " + request.path;
            return response;
        },
        files);
    std::string body(2 * 1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        body[i] = static_cast<char>(i * 7 + i / 251);
    }
    std::string uploadResponse;
    std::thread uploader([&] {
        uploadResponse = httpRequest(server.port(), "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()), body);
    });
    for (int i = 0; i < 500 && !inHandler.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(inHandler.load());
    // The upload's handler is still running: another request is answered.
    CHECK(httpRequest(server.port(), "GET /livez HTTP/1.1\r\nHost: x").find("done /livez") != std::string::npos);
    {
        std::lock_guard lock{mu};
        release = true;
    }
    cv.notify_all();
    uploader.join();
    CHECK(uploadResponse.find("200 OK") != std::string::npos);
    CHECK(seenBody.empty());
    CHECK(seenPathContent == body);
    CHECK(filesIn(dir) == 0); // removed after the handler

    // Larger than maxBytes: read, dropped, 413, no file.
    std::string const big(3 * 1024 * 1024 + 1, 'x');
    auto const refused = httpRequest(server.port(), "POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(big.size()), big);
    CHECK(refused.find("413 Payload Too Large") != std::string::npos);
    CHECK(refused.find("too large for the test") != std::string::npos);
    CHECK(filesIn(dir) == 0);
    // Other bodies stay in memory.
    CHECK(httpRequest(server.port(), "POST /other HTTP/1.1\r\nHost: x\r\nContent-Length: 3", "abc").find("done /other") != std::string::npos);
    server.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("the upload API takes a body file")
{
    auto const dir = freshUploadDir("mxl-replay-upload-api");
    auto engine = uploadEngine(dir);
    writeAv(dir / "clip.nut", 25, false);
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/uploads";
    request.query = "name=from-file";
    request.bodyPath = (dir / "clip.nut").string();
    // The log lines (1.8.3): upload_stored with what was stored, upload_failed with the answer.
    std::ostringstream log;
    auto* const cerr = std::cerr.rdbuf(log.rdbuf());
    auto const response = handleApi(*engine, request, "");
    CHECK(response.status == 201);
    CHECK(response.body.find("\"kind\":\"clip\"") != std::string::npos);
    request.bodyPath.clear();
    CHECK(handleApi(*engine, request, "").status == 400); // no body at all
    request.body = "not a media file";
    CHECK(handleApi(*engine, request, "").status == 400);
    std::cerr.rdbuf(cerr);
    auto const lines = log.str();
    auto const size = std::to_string(std::filesystem::file_size(dir / "clip.nut"));
    CHECK(lines.find("\"event\":\"upload_stored\",\"id\":\"upload-") != std::string::npos);
    CHECK(lines.find("\"kind\":\"clip\",\"name\":\"from-file\",\"bytes\":\"" + size + "\",\"frames\":\"") != std::string::npos);
    CHECK(lines.find("\"convert_ms\":\"") != std::string::npos);
    CHECK(lines.find("\"peak_rss_mb\":\"") != std::string::npos);
    CHECK(lines.find("\"event\":\"upload_failed\",\"name\":\"from-file\",\"bytes\":\"0\",\"http_status\":\"400\",\"reason\":\"the body must be the file\"") !=
          std::string::npos);
    // A body in memory (not through the server's body file) is logged as started here.
    CHECK(lines.find("\"event\":\"upload_started\",\"name\":\"from-file\",\"bytes\":\"16\",\"content_type\":\"\"") != std::string::npos);
    CHECK(lines.find("\"bytes\":\"16\",\"http_status\":\"400\"") != std::string::npos);
    CHECK(loadConfig({{"REPLAY_UPLOAD_MAX_MB", "0"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}).config.uploadMaxMb == 0);
    CHECK(loadConfig({{"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}).config.uploadMaxMb == 8192);
    CHECK_THROWS_AS((void)loadConfig({{"REPLAY_UPLOAD_MAX_MB", "-1"}, {"NMOS_HOST_ADDRESS", "10.0.0.1"}}, {}), ConfigError);
    engine.reset();
    std::filesystem::remove_all(dir);
}

namespace
{
// Sends `head` (and `body`), then reads the answer; `waitMs` before the body (Expect: 100-continue).
std::string httpExchange(int port, std::string const& head, std::string const& body, bool readFirst)
{
    int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    timeval timeout{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return {};
    }
    auto const request = head + "\r\n\r\n";
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string response;
    char buf[4096];
    if (readFirst)
    {
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            response.append(buf, buf + n);
        }
        if (response.rfind("HTTP/1.1 100", 0) != 0)
        {
            ::close(fd);
            return response; // answered before the body
        }
    }
    ::send(fd, body.data(), body.size(), MSG_NOSIGNAL);
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
    {
        response.append(buf, buf + n);
    }
    ::close(fd);
    return response;
}

std::vector<std::uint8_t> smallJpeg()
{
    Frame10 still;
    still.allocate(64, 32);
    still.fill(700, 512, 512);
    return encodeJpeg422(still, 90);
}
} // namespace

TEST_CASE("http server checks upload requests: length, chunked, one at a time, 100-continue, a stalled body")
{
    auto const dir = freshUploadDir("mxl-replay-body-checks");
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::atomic<bool> inHandler{false};
    HttpServer server;
    HttpBodyFiles files;
    files.match = [](std::string const& method, std::string const& path) { return method == "POST" && path == "/upload"; };
    files.directory = dir.string();
    files.maxActive = 1;
    files.stallSeconds = 1;
    files.admit = [](HttpRequest const&, std::uint64_t length) -> std::optional<HttpResponse> {
        if (length != 999)
        {
            return std::nullopt;
        }
        HttpResponse full;
        full.status = 507;
        full.body = "no room";
        return full;
    };
    // What the hooks saw: "started <length>" and "failed <status> <reason>".
    std::mutex eventsMu;
    std::vector<std::string> events;
    files.started = [&](HttpRequest const&, std::uint64_t length) {
        std::lock_guard lock{eventsMu};
        events.push_back("started " + std::to_string(length));
    };
    files.failed = [&](HttpRequest const&, std::uint64_t length, int status, std::string const& reason) {
        std::lock_guard lock{eventsMu};
        events.push_back("failed " + std::to_string(length) + " " + std::to_string(status) + " " + reason);
    };
    server.start(
        0,
        [&](HttpRequest const& request) {
            if (request.query == "block")
            {
                inHandler.store(true);
                std::unique_lock lock{mu};
                cv.wait_for(lock, std::chrono::seconds(10), [&] { return release; });
            }
            HttpResponse response;
            response.body = "done " + std::to_string(request.body.size()) + " " + (request.bodyPath.empty() ? "memory" : "file");
            return response;
        },
        files);
    int const port = server.port();
    // Content-Length must be digits (any request), and the same when repeated.
    for (auto const* length : {"abc", "-5", "12x", "99999999999999999999"})
    {
        CHECK(httpRequest(port, std::string("POST /other HTTP/1.1\r\nContent-Length: ") + length, "abc").find("400 Bad Request") != std::string::npos);
    }
    CHECK(httpRequest(port, "POST /upload HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4", "abcd").find("400 Bad Request") != std::string::npos);
    CHECK(httpRequest(port, "POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked", "3\r\nabc\r\n0\r\n\r\n").find("411 Length Required") != std::string::npos);
    // admit refuses: the body is read and dropped, then 507; with Expect: 100-continue at once, no body sent.
    CHECK(httpRequest(port, "POST /upload HTTP/1.1\r\nContent-Length: 999", std::string(999, 'x')).find("507 Insufficient Storage") != std::string::npos);
    CHECK(httpExchange(port, "POST /upload HTTP/1.1\r\nContent-Length: 999\r\nExpect: 100-continue", std::string(999, 'x'), true).find("HTTP/1.1 507") == 0);
    // Accepted with Expect: 100-continue: 100 first, then the answer.
    auto const continued = httpExchange(port, "POST /upload HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue", "hello", true);
    CHECK(continued.rfind("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK", 0) == 0);
    CHECK(continued.find("done 0 file") != std::string::npos);
    // One at a time: a second upload while the first is handled gets 409.
    std::string first;
    std::thread uploader([&] { first = httpRequest(port, "POST /upload?block HTTP/1.1\r\nContent-Length: 4", "abcd"); });
    for (int i = 0; i < 500 && !inHandler.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(inHandler.load());
    auto const second = httpRequest(port, "POST /upload HTTP/1.1\r\nContent-Length: 4", "abcd");
    CHECK(second.find("409 Conflict") != std::string::npos);
    {
        std::lock_guard lock{mu};
        release = true;
    }
    cv.notify_all();
    uploader.join();
    CHECK(first.find("200 OK") != std::string::npos);
    // A body that stops arriving is dropped after stallSeconds, with its file.
    auto const stalled = httpExchange(port, "POST /upload HTTP/1.1\r\nContent-Length: 100000", "only a part", false);
    CHECK(stalled.empty()); // closed without an answer
    CHECK(filesIn(dir) == 0);
    server.stop();
    // Only the upload path's requests: /other's bad lengths are not uploads.
    CHECK(events == std::vector<std::string>{"failed 4 400 invalid Content-Length", "failed 0 411 the upload needs a Content-Length (no chunked transfer)",
                        "failed 999 507 no room", "failed 999 507 no room", "started 5", "started 4", "failed 4 409 an upload is already running; try again when it is stored",
                        "started 100000", "failed 100000 0 the body stopped arriving"});
    std::filesystem::remove_all(dir);
}

TEST_CASE("a damaged JPEG fails the decoders instead of ending the process")
{
    auto const good = smallJpeg();
    Frame10 frame;
    REQUIRE(decodeJpeg422(good.data(), good.size(), frame));
    std::vector<std::vector<std::uint8_t>> damaged;
    for (std::size_t cut : {4u, 20u, 100u, 200u})
    {
        damaged.emplace_back(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(std::min(cut, good.size())));
    }
    std::vector<std::uint8_t> junk{0xff, 0xd8};
    junk.insert(junk.end(), 500, 0x42);
    damaged.push_back(junk);
    auto badFrame = good;
    for (std::size_t i = 2; i + 1 < badFrame.size(); ++i)
    {
        if (badFrame[i] == 0xff && badFrame[i + 1] == 0xc0) // SOF0: precision 0 is an error
        {
            badFrame[i + 4] = 0;
            break;
        }
    }
    damaged.push_back(badFrame);
    std::vector<std::uint8_t> v210(static_cast<std::size_t>((64 + 5) / 6 * 16 * 32));
    for (auto const& bytes : damaged)
    {
        Frame10 out;
        (void)decodeJpeg422(bytes.data(), bytes.size(), out); // a cut-off file may still decode (grey rest)
        (void)decodeJpegPreview(bytes.data(), bytes.size(), 16, out);
        (void)decodeJpegToV210(bytes.data(), bytes.size(), 64, 32, 0, v210.data());
    }
    CHECK_FALSE(decodeJpeg422(junk.data(), junk.size(), frame));
    CHECK_FALSE(decodeJpeg422(badFrame.data(), badFrame.size(), frame));
    CHECK_FALSE(decodeJpegToV210(badFrame.data(), badFrame.size(), 64, 32, 0, v210.data()));
    // The thread's decoder still works after an error.
    CHECK(decodeJpegToV210(good.data(), good.size(), 64, 32, 0, v210.data()));
}

TEST_CASE("an upload of a damaged JPEG answers 400, a still over 64 MB 413")
{
    auto const dir = freshUploadDir("mxl-replay-upload-jpeg");
    auto engine = uploadEngine(dir);
    std::vector<std::uint8_t> junk{0xff, 0xd8};
    junk.insert(junk.end(), 500, 0x42);
    {
        std::ofstream out(dir / "junk.jpg", std::ios::binary);
        out.write(reinterpret_cast<char const*>(junk.data()), static_cast<std::streamsize>(junk.size()));
    }
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/uploads";
    request.bodyPath = (dir / "junk.jpg").string();
    CHECK(handleApi(*engine, request, "").status == 400);
    {
        std::ofstream out(dir / "big.jpg", std::ios::binary);
        out.write(reinterpret_cast<char const*>(junk.data()), 2);
        std::vector<char> pad(1024 * 1024, 0);
        for (int i = 0; i < 64; ++i)
        {
            out.write(pad.data(), static_cast<std::streamsize>(pad.size()));
        }
    }
    request.bodyPath = (dir / "big.jpg").string();
    auto const big = handleApi(*engine, request, "");
    CHECK(big.status == 413);
    CHECK(big.body.find("64 MB") != std::string::npos);
    engine.reset();
    std::filesystem::remove_all(dir);
}

TEST_CASE("discardFrom removes a failed upload's segments, the open one too, and keeps protected ones")
{
    auto const dir = freshUploadDir("mxl-replay-ring-discard");
    constexpr std::uint64_t kS = 1000000000ull;
    auto frame = [](std::uint64_t tai) {
        StoredFrame f;
        f.taiNs = tai;
        f.jpeg = {1, 2, 3, 4};
        return f;
    };
    {
        FrameRing ring(dir.string(), 1, 1); // the library: 1 ns retention, 1 s segments
        for (std::uint64_t i = 0; i < 20; ++i)
        {
            REQUIRE(ring.push(frame(100 * kS + i * kS / 10))); // a clip: 100.0 … 101.9 s
        }
        ring.protect(100 * kS, 101 * kS + 9 * kS / 10);
        auto const keptSegments = ring.segmentCount();
        auto const origin = 102 * kS;
        ring.protect(origin, std::numeric_limits<std::uint64_t>::max());
        for (std::uint64_t i = 0; i < 35; ++i)
        {
            REQUIRE(ring.push(frame(origin + i * kS / 10))); // the failed upload: 3.5 s
        }
        CHECK(ring.segmentCount() > keptSegments);
        ring.unprotect(origin, std::numeric_limits<std::uint64_t>::max());
        // The clip's last segment (101.1 … 102.1 s) also holds the upload's first two frames: it stays
        // for the clip. Every segment that starts in the upload goes, the open one included.
        CHECK(ring.discardFrom(origin) == 33);
        CHECK(ring.size() == 22);
        CHECK(ring.segmentCount() == keptSegments);
        CHECK(ring.newestNs() == origin + kS / 10);
        CHECK(ring.push(frame(origin + 2 * kS / 10))); // the next upload opens a new segment
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the janitor deletes the files
    FrameRing again(dir.string(), 1, 1);
    CHECK(again.size() == 23);
    std::filesystem::remove_all(dir);
}
