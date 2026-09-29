#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tech_news {
enum class ContentKind { Summary = 0, Feed = 1, Extracted = 2, Incomplete = 3 };

struct Story {
  std::string url;
  std::string title;
  std::string summary;
  std::string content;
  std::string source;
  std::string language;
  std::int64_t published = 0;
  std::int64_t seen = 0;
  ContentKind kind = ContentKind::Summary;
  bool read = false;
  bool bookmarked = false;
};

struct Feed {
  std::string name;
  std::string source;
  std::string language;
  std::string url;
};

inline const std::vector<Feed>& curated_feeds() {
  static const std::vector<Feed> feeds = {
    {"Ars / Technology", "Ars Technica", "en", "https://feeds.arstechnica.com/arstechnica/technology-lab"},
    {"Ars / Gadgets", "Ars Technica", "en", "https://feeds.arstechnica.com/arstechnica/gadgets"},
    {"TechCrunch / AI", "TechCrunch", "en", "https://techcrunch.com/category/artificial-intelligence/feed/"},
    {"TechCrunch / Security", "TechCrunch", "en", "https://techcrunch.com/category/security/feed/"},
    {"Tecnoblog / News", "Tecnoblog", "pt-BR", "https://tecnoblog.net/noticias/feed/"},
  };
  return feeds;
}

struct Filter {
  std::string source;
  std::string language;
  std::string search;
  bool unread_only = false;
  bool bookmarked_only = false;
};

std::vector<Story> merge_stories(std::vector<Story> stories);
std::vector<std::size_t> filter_stories(const std::vector<Story>& stories, const Filter& filter);
bool technology_story(const Feed& feed, const std::string& url, const std::vector<std::string>& categories);
std::int64_t now_seconds();
}  // namespace tech_news
