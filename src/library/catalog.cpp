#include "library/catalog.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

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
        "CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS audio_items (id TEXT PRIMARY KEY, name TEXT, samples INTEGER, created_ns INTEGER);");
    // Columns added since 1.0 (1.6.0: a playlist can loop, transitions, own audio and the aux track): a
    // catalog from before gets them with their defaults.
    auto const addColumns = [this](char const* table, std::vector<std::pair<char const*, char const*>> const& columns) {
        std::vector<std::string> present;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, (std::string("PRAGMA table_info(") + table + ")").c_str(), -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            auto const* column = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
            present.emplace_back(column != nullptr ? column : "");
        }
        sqlite3_finalize(stmt);
        for (auto const& [name, type] : columns)
        {
            if (std::find(present.begin(), present.end(), name) == present.end())
            {
                exec(db_, (std::string("ALTER TABLE ") + table + " ADD COLUMN " + name + " " + type + ";").c_str());
            }
        }
    };
    addColumns("playlists", {{"loop", "INTEGER NOT NULL DEFAULT 0"}, {"aux_source", "TEXT NOT NULL DEFAULT ''"}, {"aux_level_db", "REAL NOT NULL DEFAULT 0"},
                                {"aux_loop", "INTEGER NOT NULL DEFAULT 1"}, {"aux_fade_frames", "INTEGER NOT NULL DEFAULT 50"},
                                {"aux_duck_db", "REAL NOT NULL DEFAULT -12"}, {"aux_attack_ms", "INTEGER NOT NULL DEFAULT 200"},
                                {"aux_release_ms", "INTEGER NOT NULL DEFAULT 500"}});
    addColumns("playlist_entries", {{"transition", "TEXT NOT NULL DEFAULT 'cut'"}, {"transition_frames", "INTEGER NOT NULL DEFAULT 0"},
                                       {"audio_frames", "INTEGER NOT NULL DEFAULT -1"}, {"own_audio", "INTEGER NOT NULL DEFAULT 1"}});
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

namespace
{
constexpr char const* kClipColumns = "SELECT id,name,camera,in_ns,out_ns,speed,motion,audio,colour,tags,end_action,group_id,library FROM clips";

// One row of kClipColumns.
ClipRef readClip(sqlite3_stmt* stmt)
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
    return clip;
}
} // namespace

std::vector<ClipRef> Catalog::clips() const
{
    std::vector<ClipRef> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, (std::string(kClipColumns) + " ORDER BY name").c_str(), -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        out.push_back(readClip(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

ClipRef Catalog::clip(std::string const& id) const
{
    ClipRef clip;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, (std::string(kClipColumns) + " WHERE id=?").c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        clip = readClip(stmt);
    }
    sqlite3_finalize(stmt);
    return clip;
}

void Catalog::upsertPlaylist(Playlist const& playlist)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO playlists (id,name,loop,aux_source,aux_level_db,aux_loop,aux_fade_frames,aux_duck_db,aux_attack_ms,aux_release_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,loop=excluded.loop,aux_source=excluded.aux_source,"
        "aux_level_db=excluded.aux_level_db,aux_loop=excluded.aux_loop,aux_fade_frames=excluded.aux_fade_frames,aux_duck_db=excluded.aux_duck_db,"
        "aux_attack_ms=excluded.aux_attack_ms,aux_release_ms=excluded.aux_release_ms",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, playlist.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, playlist.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, playlist.loop ? 1 : 0);
    sqlite3_bind_text(stmt, 4, playlist.aux.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, playlist.aux.levelDb);
    sqlite3_bind_int(stmt, 6, playlist.aux.loop ? 1 : 0);
    sqlite3_bind_int(stmt, 7, playlist.aux.fadeOutFrames);
    sqlite3_bind_double(stmt, 8, playlist.aux.duckDb);
    sqlite3_bind_int(stmt, 9, playlist.aux.attackMs);
    sqlite3_bind_int(stmt, 10, playlist.aux.releaseMs);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db_, "DELETE FROM playlist_entries WHERE playlist=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, playlist.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db_, "INSERT INTO playlist_entries (playlist,position,clip_id,speed,end_action,auto_advance,transition,transition_frames,audio_frames,own_audio) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr);
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
        sqlite3_bind_text(stmt, 7, transitionName(entry.transition), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, entry.transitionFrames);
        sqlite3_bind_int(stmt, 9, entry.audioFrames);
        sqlite3_bind_int(stmt, 10, entry.ownAudio ? 1 : 0);
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
    sqlite3_prepare_v2(db_, "SELECT id,name,loop,aux_source,aux_level_db,aux_loop,aux_fade_frames,aux_duck_db,aux_attack_ms,aux_release_ms FROM playlists ORDER BY name", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Playlist playlist;
        playlist.id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        playlist.name = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
        playlist.loop = sqlite3_column_int(stmt, 2) != 0;
        auto const* source = sqlite3_column_text(stmt, 3);
        playlist.aux.source = source != nullptr ? reinterpret_cast<char const*>(source) : "";
        playlist.aux.levelDb = sqlite3_column_double(stmt, 4);
        playlist.aux.loop = sqlite3_column_int(stmt, 5) != 0;
        playlist.aux.fadeOutFrames = sqlite3_column_int(stmt, 6);
        playlist.aux.duckDb = sqlite3_column_double(stmt, 7);
        playlist.aux.attackMs = sqlite3_column_int(stmt, 8);
        playlist.aux.releaseMs = sqlite3_column_int(stmt, 9);
        out.push_back(std::move(playlist));
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db_, "SELECT playlist,clip_id,speed,end_action,auto_advance,transition,transition_frames,audio_frames,own_audio FROM playlist_entries ORDER BY position", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string const id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        PlaylistEntry entry;
        entry.clipId = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
        entry.speed = sqlite3_column_double(stmt, 2);
        entry.end = parseEndAction(reinterpret_cast<char const*>(sqlite3_column_text(stmt, 3)));
        entry.autoAdvance = sqlite3_column_int(stmt, 4) != 0;
        auto const* transition = sqlite3_column_text(stmt, 5);
        entry.transition = parseTransition(transition != nullptr ? reinterpret_cast<char const*>(transition) : "cut");
        entry.transitionFrames = sqlite3_column_int(stmt, 6);
        entry.audioFrames = sqlite3_column_int(stmt, 7);
        entry.ownAudio = sqlite3_column_int(stmt, 8) != 0;
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

void Catalog::upsertAudioItem(AudioItem const& item)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO audio_items (id,name,samples,created_ns) VALUES (?,?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,samples=excluded.samples",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, item.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, item.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, item.samples);
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(item.createdNs));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Catalog::deleteAudioItem(std::string const& id)
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM audio_items WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Catalog::AudioItem> Catalog::audioItems() const
{
    std::vector<AudioItem> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT id,name,samples,created_ns FROM audio_items ORDER BY name", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        AudioItem item;
        item.id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        auto const* name = sqlite3_column_text(stmt, 1);
        item.name = name != nullptr ? reinterpret_cast<char const*>(name) : "";
        item.samples = sqlite3_column_int64(stmt, 2);
        item.createdNs = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
        out.push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
    return out;
}

Catalog::AudioItem Catalog::audioItem(std::string const& id) const
{
    for (auto& item : audioItems())
    {
        if (item.id == id)
        {
            return item;
        }
    }
    return {};
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
