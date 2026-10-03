#include "library/catalog.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace replay
{
namespace
{
void exec(sqlite3* db, char const* sql)
{
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        std::string message = err != nullptr ? err : "sqlite error";
        sqlite3_free(err);
        throw std::runtime_error(message);
    }
}
} // namespace

Catalog::Catalog() = default;

Catalog::~Catalog()
{
    close();
}

void Catalog::open(std::string const& path)
{
    close();
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
    {
        std::string message = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(message);
    }
    exec(db_, "PRAGMA journal_mode=WAL;");
    exec(db_,
        "CREATE TABLE IF NOT EXISTS clips ("
        "id TEXT PRIMARY KEY, name TEXT, camera INTEGER, in_ns INTEGER, out_ns INTEGER, speed REAL,"
        "motion TEXT, audio TEXT, colour TEXT, tags TEXT, end_action TEXT, group_id TEXT, library INTEGER);"
        "CREATE TABLE IF NOT EXISTS playlists (id TEXT PRIMARY KEY, name TEXT);"
        "CREATE TABLE IF NOT EXISTS playlist_entries (playlist TEXT, position INTEGER, clip_id TEXT, speed REAL, end_action TEXT, auto_advance INTEGER);"
        "CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT);");
}

void Catalog::close()
{
    if (db_ != nullptr)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Catalog::upsertClip(ClipRef const& clip)
{
    sqlite3_stmt* stmt = nullptr;
    char const* sql =
        "INSERT INTO clips (id,name,camera,in_ns,out_ns,speed,motion,audio,colour,tags,end_action,group_id,library) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name, camera=excluded.camera, in_ns=excluded.in_ns, out_ns=excluded.out_ns,"
        "speed=excluded.speed, motion=excluded.motion, audio=excluded.audio, colour=excluded.colour, tags=excluded.tags,"
        "end_action=excluded.end_action, group_id=excluded.group_id, library=excluded.library";
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, clip.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, clip.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, clip.camera);
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(clip.inNs));
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(clip.outNs));
    sqlite3_bind_double(stmt, 6, clip.speed);
    sqlite3_bind_text(stmt, 7, clip.motion.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, clip.audio.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, clip.colour.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, clip.tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, endActionName(clip.end), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, clip.groupId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, clip.library ? 1 : 0);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Catalog::deleteClip(std::string const& id)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM clips WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<ClipRef> Catalog::clips() const
{
    std::vector<ClipRef> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT id,name,camera,in_ns,out_ns,speed,motion,audio,colour,tags,end_action,group_id,library FROM clips ORDER BY name", -1,
        &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        ClipRef clip;
        clip.id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        clip.name = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
        clip.camera = sqlite3_column_int(stmt, 2);
        clip.inNs = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        clip.outNs = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
        clip.speed = sqlite3_column_double(stmt, 5);
        clip.motion = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 6));
        clip.audio = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 7));
        clip.colour = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 8));
        auto const tags = sqlite3_column_text(stmt, 9);
        clip.tags = tags != nullptr ? reinterpret_cast<char const*>(tags) : "";
        clip.end = parseEndAction(reinterpret_cast<char const*>(sqlite3_column_text(stmt, 10)));
        auto const group = sqlite3_column_text(stmt, 11);
        clip.groupId = group != nullptr ? reinterpret_cast<char const*>(group) : "";
        clip.library = sqlite3_column_int(stmt, 12) != 0;
        out.push_back(std::move(clip));
    }
    sqlite3_finalize(stmt);
    return out;
}

ClipRef Catalog::clip(std::string const& id) const
{
    for (auto const& clip : clips())
    {
        if (clip.id == id)
        {
            return clip;
        }
    }
    return {};
}

void Catalog::upsertPlaylist(Playlist const& playlist)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO playlists (id,name) VALUES (?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, playlist.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, playlist.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db_, "DELETE FROM playlist_entries WHERE playlist=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, playlist.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db_, "INSERT INTO playlist_entries (playlist,position,clip_id,speed,end_action,auto_advance) VALUES (?,?,?,?,?,?)", -1, &stmt, nullptr);
    for (std::size_t i = 0; i < playlist.entries.size(); ++i)
    {
        auto const& entry = playlist.entries[i];
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, playlist.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, static_cast<int>(i));
        sqlite3_bind_text(stmt, 3, entry.clipId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, entry.speed);
        sqlite3_bind_text(stmt, 5, endActionName(entry.end), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 6, entry.autoAdvance ? 1 : 0);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

void Catalog::deletePlaylist(std::string const& id)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM playlists WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db_, "DELETE FROM playlist_entries WHERE playlist=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Catalog::Playlist> Catalog::playlists() const
{
    std::vector<Playlist> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT id,name FROM playlists ORDER BY name", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Playlist playlist;
        playlist.id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        playlist.name = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
        out.push_back(std::move(playlist));
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db_, "SELECT playlist,clip_id,speed,end_action,auto_advance FROM playlist_entries ORDER BY position", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string const id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        PlaylistEntry entry;
        entry.clipId = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
        entry.speed = sqlite3_column_double(stmt, 2);
        entry.end = parseEndAction(reinterpret_cast<char const*>(sqlite3_column_text(stmt, 3)));
        entry.autoAdvance = sqlite3_column_int(stmt, 4) != 0;
        for (auto& playlist : out)
        {
            if (playlist.id == id)
            {
                playlist.entries.push_back(entry);
            }
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

void Catalog::setState(std::string const& key, std::string const& value)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO state (key,value) VALUES (?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string Catalog::state(std::string const& key) const
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT value FROM state WHERE key=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string value;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0) != nullptr)
    {
        value = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return value;
}
} // namespace replay
