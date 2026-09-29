#include "tech_news/store.hpp"
#include "tech_news/text.hpp"
#include <sqlite3.h>
#include <memory>
#include <stdexcept>

namespace tech_news {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
void execute(sqlite3* db, const char* sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error("Cache operation failed (check storage space and permissions)");
}
Statement prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) throw std::runtime_error("Could not prepare cache query");
  return Statement(raw, sqlite3_finalize);
}
void bind(sqlite3_stmt* stmt, int index, const std::string& value) {
  if (sqlite3_bind_text(stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
    throw std::runtime_error("Could not bind cache text");
}
void bind(sqlite3_stmt* stmt, int index, std::int64_t value) {
  if (sqlite3_bind_int64(stmt, index, value) != SQLITE_OK) throw std::runtime_error("Could not bind cache value");
}
void done(sqlite3_stmt* stmt) {
  if (sqlite3_step(stmt) != SQLITE_DONE) throw std::runtime_error("Could not write cache (check storage space and permissions)");
}
std::string column(sqlite3_stmt* stmt, int index) {
  const auto* value = sqlite3_column_text(stmt, index);
  return value ? sanitize_text(std::string_view(reinterpret_cast<const char*>(value), static_cast<std::size_t>(sqlite3_column_bytes(stmt, index)))) : "";
}
}  // namespace

Store::Store(const std::filesystem::path& path) {
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
  flags |= SQLITE_OPEN_NOFOLLOW;
#endif
  if (sqlite3_open_v2(path.string().c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("Could not open local cache (check storage permissions)");
  }
  try {
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    sqlite3_busy_timeout(db_, 1000);
    auto version = prepare(db_, "PRAGMA user_version");
    if (sqlite3_step(version.get()) != SQLITE_ROW || sqlite3_column_int(version.get(), 0) > 1)
      throw std::runtime_error("Cache was created by a newer version of tech-news");
    version.reset();
    execute(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA trusted_schema=OFF;");
    execute(db_, "CREATE TABLE IF NOT EXISTS stories("
      "url TEXT PRIMARY KEY,title TEXT NOT NULL,summary TEXT NOT NULL,content TEXT NOT NULL,"
      "source TEXT NOT NULL,language TEXT NOT NULL,published INTEGER NOT NULL,seen INTEGER NOT NULL,"
      "kind INTEGER NOT NULL,read INTEGER NOT NULL DEFAULT 0,bookmarked INTEGER NOT NULL DEFAULT 0);"
      "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS stories_published ON stories(published DESC); PRAGMA user_version=1;");
  } catch (const std::filesystem::filesystem_error&) {
    sqlite3_close(db_); db_ = nullptr; throw std::runtime_error("Could not set private cache permissions");
  } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
Store::~Store() { if (db_) sqlite3_close(db_); }

std::vector<Story> Store::load() const {
  auto stmt = prepare(db_, "SELECT url,title,summary,content,source,language,published,seen,kind,read,bookmarked FROM stories ORDER BY published DESC,url");
  std::vector<Story> result;
  int code;
  while ((code = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    Story story;
    story.url = normalize_url(column(stmt.get(), 0));
    if (story.url.empty()) continue;
    story.title = single_line(column(stmt.get(), 1)); story.summary = column(stmt.get(), 2);
    story.content = column(stmt.get(), 3); story.source = single_line(column(stmt.get(), 4)); story.language = single_line(column(stmt.get(), 5));
    story.published = sqlite3_column_int64(stmt.get(), 6); story.seen = sqlite3_column_int64(stmt.get(), 7);
    auto kind = sqlite3_column_int(stmt.get(), 8);
    story.kind = kind >= 0 && kind <= 3 ? static_cast<ContentKind>(kind) : ContentKind::Summary;
    story.read = sqlite3_column_int(stmt.get(), 9) != 0; story.bookmarked = sqlite3_column_int(stmt.get(), 10) != 0;
    result.push_back(std::move(story));
  }
  if (code != SQLITE_DONE) throw std::runtime_error("Could not read cache");
  return result;
}

void Store::upsert(const std::vector<Story>& stories) {
  auto stmt = prepare(db_, "INSERT INTO stories(url,title,summary,content,source,language,published,seen,kind,read,bookmarked)"
    " VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(url) DO UPDATE SET title=excluded.title,summary=excluded.summary,"
    "source=excluded.source,language=excluded.language,seen=CASE WHEN stories.seen>0 THEN stories.seen ELSE excluded.seen END,"
    "published=CASE WHEN excluded.published>0 THEN excluded.published ELSE stories.published END,"
    "content=CASE WHEN stories.kind IN (1,2) THEN stories.content WHEN length(excluded.content)>length(stories.content) THEN excluded.content ELSE stories.content END,"
    "kind=CASE WHEN stories.kind IN (1,2) THEN stories.kind WHEN length(excluded.content)>length(stories.content) THEN excluded.kind ELSE stories.kind END");
  execute(db_, "BEGIN IMMEDIATE");
  try {
    for (const auto& s : stories) {
      const auto url = normalize_url(s.url);
      if (url.empty()) continue;
      sqlite3_reset(stmt.get()); sqlite3_clear_bindings(stmt.get());
      bind(stmt.get(), 1, url); bind(stmt.get(), 2, single_line(s.title)); bind(stmt.get(), 3, sanitize_text(s.summary));
      bind(stmt.get(), 4, sanitize_text(s.content)); bind(stmt.get(), 5, single_line(s.source)); bind(stmt.get(), 6, single_line(s.language));
      bind(stmt.get(), 7, s.published); bind(stmt.get(), 8, s.seen); bind(stmt.get(), 9, static_cast<std::int64_t>(s.kind));
      bind(stmt.get(), 10, std::int64_t{s.read}); bind(stmt.get(), 11, std::int64_t{s.bookmarked});
      done(stmt.get());
    }
    execute(db_, "COMMIT");
  } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}
void Store::set_flags(const std::string& url, bool read, bool bookmarked) {
  auto stmt = prepare(db_, "UPDATE stories SET read=?,bookmarked=? WHERE url=?");
  bind(stmt.get(), 1, std::int64_t{read}); bind(stmt.get(), 2, std::int64_t{bookmarked}); bind(stmt.get(), 3, url); done(stmt.get());
}
void Store::save_content(const std::string& url, const std::string& content, ContentKind kind) {
  auto stmt = prepare(db_, "UPDATE stories SET content=?,kind=? WHERE url=?");
  bind(stmt.get(), 1, sanitize_text(content)); bind(stmt.get(), 2, static_cast<std::int64_t>(kind)); bind(stmt.get(), 3, url); done(stmt.get());
}
void Store::prune(std::int64_t now) {
  auto stmt = prepare(db_, "DELETE FROM stories WHERE bookmarked=0 AND CASE WHEN published>0 THEN published ELSE seen END<?");
  bind(stmt.get(), 1, now - 30 * 86400); done(stmt.get());
}
std::int64_t Store::last_refresh() const {
  auto stmt = prepare(db_, "SELECT value FROM metadata WHERE key='last_refresh'");
  int code = sqlite3_step(stmt.get());
  if (code == SQLITE_DONE) return 0;
  if (code != SQLITE_ROW) throw std::runtime_error("Could not read refresh timestamp");
  return sqlite3_column_int64(stmt.get(), 0);
}
void Store::set_last_refresh(std::int64_t now) {
  auto stmt = prepare(db_, "INSERT INTO metadata(key,value) VALUES('last_refresh',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
  bind(stmt.get(), 1, now); done(stmt.get());
}
}  // namespace tech_news
