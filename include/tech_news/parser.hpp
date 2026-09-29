#pragma once
#include "tech_news/model.hpp"
#include <string_view>

namespace tech_news {
struct ArticleText {
  std::string text;
  ContentKind kind = ContentKind::Summary;
};
std::vector<Story> parse_feed(std::string_view xml, const Feed& feed, std::int64_t now);
std::string html_to_text(std::string_view html);
ArticleText extract_article(std::string_view html, std::string_view url);
}  // namespace tech_news
