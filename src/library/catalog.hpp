#pragma once

#include "playout/shotbox.hpp"

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
    };
    void upsertPlaylist(Playlist const& playlist);
    void deletePlaylist(std::string const& id);
    [[nodiscard]] std::vector<Playlist> playlists() const;

    void setState(std::string const& key, std::string const& value);
    [[nodiscard]] std::string state(std::string const& key) const;

private:
    sqlite3* db_ = nullptr;
};
} // namespace replay
