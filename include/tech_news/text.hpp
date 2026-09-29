#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace tech_news {
std::string sanitize_text(std::string_view input);
std::string single_line(std::string_view input);
std::string search_key(std::string_view input);
std::string normalize_url(std::string_view url);
std::string resolve_url(std::string_view base, std::string_view relative);
std::string url_host(std::string_view url);
std::int64_t parse_date(std::string_view date);
std::string display_date(std::int64_t date);
}  // namespace tech_news
