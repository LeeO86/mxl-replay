#pragma once

#include "playout/shotbox.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace replay
{
class Catalog
{
public:
    Catalog();
    ~Catalog();
    Catalog(Catalog const&) = delete;
    Catalog& operator=(Catalog const&) = delete;

    void open(std::string const& path);
    void close();

    void upsertClip(ClipRef const& clip);
    void deleteClip(std::string const& id);
    [[nodiscard]] std::vector<ClipRef> clips() const;
    [[nodiscard]] ClipRef clip(std::string const& id) const;

    struct Playlist
    {
        std::string id;
        std::string name;
        std::vector<PlaylistEntry> entries;
        // After its last entry the playlist starts again at its first (SPECIFICATION.md §7).
        bool loop = false;
        // The audio aux track (1.6.0; no source: none).
        AuxTrack aux;
    };
    void upsertPlaylist(Playlist const& playlist);
    void deletePlaylist(std::string const& id);
    [[nodiscard]] std::vector<Playlist> playlists() const;

    // An audio-only library item (1.6.0): 48 kHz stereo float PCM in a file of the library.
    struct AudioItem
    {
        std::string id;
        std::string name;
        // Sample frames (per channel).
        std::int64_t samples = 0;
        std::uint64_t createdNs = 0;
    };
    void upsertAudioItem(AudioItem const& item);
    void deleteAudioItem(std::string const& id);
    [[nodiscard]] std::vector<AudioItem> audioItems() const;
    // Empty id when unknown.
    [[nodiscard]] AudioItem audioItem(std::string const& id) const;

    void setState(std::string const& key, std::string const& value);
    [[nodiscard]] std::string state(std::string const& key) const;

private:
    sqlite3* db_ = nullptr;
};
} // namespace replay
