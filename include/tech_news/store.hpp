#pragma once
#include "tech_news/model.hpp"
#include <filesystem>
#include <string>

struct sqlite3;
namespace tech_news {
class Store {
 public:
  explicit Store(const std::filesystem::path& path);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  std::vector<Story> load() const;
  void upsert(const std::vector<Story>& stories);
  void set_flags(const std::string& url, bool read, bool bookmarked);
  void save_content(const std::string& url, const std::string& content, ContentKind kind);
  void prune(std::int64_t now);
  std::int64_t last_refresh() const;
  void set_last_refresh(std::int64_t now);
 private:
  sqlite3* db_ = nullptr;
};
}  // namespace tech_news
