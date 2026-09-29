#include "tech_news/text.hpp"
#include "tech_news/model.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace tech_news {
namespace {
using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
std::string part(CURLU* url, CURLUPart field, unsigned flags = 0) {
  char* value = nullptr;
  if (curl_url_get(url, field, &value, flags) != CURLUE_OK) return {};
  std::unique_ptr<char, decltype(&curl_free)> owner(value, curl_free);
  return value;
}
std::string lower_ascii(std::string str) {
  for (auto& ch : str) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return str;
}
bool url_chars(std::string_view url) {
  if (url.size() > 8192) return false;
  for (unsigned char ch : url)
    if (ch <= 32 || ch == 127 || ch == '\\' || ch == '"' || ch == '<' || ch == '>') return false;
  return true;
}
std::pair<std::uint32_t, std::size_t> decode(std::string_view str, std::size_t i) {
  const auto first = static_cast<unsigned char>(str[i]);
  if (first < 128) return {first, 1};
  std::size_t count = first >= 0xc2 && first <= 0xdf ? 2 :
                      first >= 0xe0 && first <= 0xef ? 3 :
                      first >= 0xf0 && first <= 0xf4 ? 4 : 0;
  if (!count || i + count > str.size()) return {0xfffd, 1};
  std::uint32_t cp = first & (0x7fU >> count);
  for (std::size_t j = 1; j < count; ++j) {
    auto ch = static_cast<unsigned char>(str[i + j]);
    if ((ch & 0xc0U) != 0x80U) return {0xfffd, 1};
    cp = (cp << 6U) | (ch & 0x3fU);
  }
  if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) ||
      (count == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return {0xfffd, 1};
  return {cp, count};
}
void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 128) out += static_cast<char>(cp);
  else if (cp < 2048) {
    out += static_cast<char>(0xc0U | (cp >> 6U)); out += static_cast<char>(0x80U | (cp & 63U));
  } else if (cp < 65536) {
    out += static_cast<char>(0xe0U | (cp >> 12U)); out += static_cast<char>(0x80U | ((cp >> 6U) & 63U));
    out += static_cast<char>(0x80U | (cp & 63U));
  } else {
    out += static_cast<char>(0xf0U | (cp >> 18U)); out += static_cast<char>(0x80U | ((cp >> 12U) & 63U));
    out += static_cast<char>(0x80U | ((cp >> 6U) & 63U)); out += static_cast<char>(0x80U | (cp & 63U));
  }
}
std::size_t skip_csi(std::string_view str, std::size_t i) {
  while (i < str.size()) { auto ch = static_cast<unsigned char>(str[i++]); if (ch >= 0x40 && ch <= 0x7e) break; }
  return i;
}
std::size_t skip_string(std::string_view str, std::size_t i) {
  while (i < str.size()) {
    if (str[i] == '\a') return i + 1;
    if (str[i] == '\033' && i + 1 < str.size() && str[i + 1] == '\\') return i + 2;
    auto [cp, size] = decode(str, i); i += size;
    if (cp == 0x9c) return i;
  }
  return i;
}
}  // namespace

std::string sanitize_text(std::string_view input) {
  std::string out;
  out.reserve(std::min(input.size(), std::size_t{512000}));
  for (std::size_t i = 0; i < input.size() && out.size() < 512000;) {
    auto [cp, size] = decode(input, i); i += size;
    if (cp == 27) {
      if (i == input.size()) break;
      char command = input[i++];
      if (command == '[') i = skip_csi(input, i);
      else if (command == ']' || command == 'P' || command == 'X' || command == '^' || command == '_')
        i = skip_string(input, i);
      else if (command >= ' ' && command <= '/') {
        while (i < input.size() && input[i] >= ' ' && input[i] <= '/') ++i;
        if (i < input.size()) ++i;
      }
      continue;
    }
    if (cp == 0x9b) { i = skip_csi(input, i); continue; }
    if (cp == 0x9d || cp == 0x90 || cp == 0x98 || cp == 0x9e || cp == 0x9f) {
      i = skip_string(input, i); continue;
    }
    if (cp == '\r') { if (i == input.size() || input[i] != '\n') out += '\n'; continue; }
    if (cp == '\t' || cp == 0xa0) { out += ' '; continue; }
    if (cp == '\n') { out += '\n'; continue; }
    if (cp < 32 || (cp >= 127 && cp <= 159) || cp == 0x2028 || cp == 0x2029 ||
        (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200b || cp == 0xfeff) continue;
    append_utf8(out, cp);
  }
  return out;
}

std::string single_line(std::string_view input) {
  auto clean = sanitize_text(input);
  std::string out;
  bool space = false;
  for (char ch : clean) {
    if (ch == ' ' || ch == '\n') { space = !out.empty(); continue; }
    if (space) out += ' ';
    space = false; out += ch;
  }
  return out;
}

std::string search_key(std::string_view input) {
  auto clean = single_line(input);
  std::string out;
  for (std::size_t i = 0; i < clean.size();) {
    auto [cp, size] = decode(clean, i); i += size;
    if (cp >= 'A' && cp <= 'Z') cp += 32;
    if (cp >= 0xc0 && cp <= 0xde && cp != 0xd7) cp += 32;
    switch (cp) {
      case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: cp = 'a'; break;
      case 0xe8: case 0xe9: case 0xea: case 0xeb: cp = 'e'; break;
      case 0xec: case 0xed: case 0xee: case 0xef: cp = 'i'; break;
      case 0xf2: case 0xf3: case 0xf4: case 0xf5: case 0xf6: cp = 'o'; break;
      case 0xf9: case 0xfa: case 0xfb: case 0xfc: cp = 'u'; break;
      case 0xe7: cp = 'c'; break;
    }
    append_utf8(out, cp);
  }
  return out;
}

std::string normalize_url(std::string_view input) {
  if (!url_chars(input)) return {};
  Url url(curl_url(), curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, std::string(input).c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return {};
  auto scheme = lower_ascii(part(url.get(), CURLUPART_SCHEME));
  if (scheme != "http" && scheme != "https") return {};
  auto host = lower_ascii(part(url.get(), CURLUPART_HOST));
  if (host.empty()) return {};
  curl_url_set(url.get(), CURLUPART_SCHEME, scheme.c_str(), 0);
  curl_url_set(url.get(), CURLUPART_HOST, host.c_str(), 0);
  curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0);
  auto path = part(url.get(), CURLUPART_PATH);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  curl_url_set(url.get(), CURLUPART_PATH, path.c_str(), 0);
  auto query = part(url.get(), CURLUPART_QUERY);
  std::vector<std::string> args;
  std::istringstream stream(query);
  std::string arg;
  while (std::getline(stream, arg, '&')) {
    auto key = lower_ascii(arg.substr(0, arg.find('=')));
    if (arg.empty() || key.starts_with("utm_") || key == "fbclid" || key == "gclid" ||
        key == "mc_cid" || key == "mc_eid" || key == "_ga") continue;
    args.push_back(arg);
  }
  std::sort(args.begin(), args.end());
  query.clear();
  for (const auto& value : args) { if (!query.empty()) query += '&'; query += value; }
  curl_url_set(url.get(), CURLUPART_QUERY, query.empty() ? nullptr : query.c_str(), 0);
  return part(url.get(), CURLUPART_URL, CURLU_NO_DEFAULT_PORT);
}

std::string resolve_url(std::string_view base, std::string_view relative) {
  if (!url_chars(relative)) return {};
  Url url(curl_url(), curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, std::string(base).c_str(), CURLU_DISALLOW_USER) != CURLUE_OK ||
      curl_url_set(url.get(), CURLUPART_URL, std::string(relative).c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return {};
  return normalize_url(part(url.get(), CURLUPART_URL));
}

std::string url_host(std::string_view input) {
  auto normalized = normalize_url(input);
  Url url(curl_url(), curl_url_cleanup);
  if (normalized.empty() || !url || curl_url_set(url.get(), CURLUPART_URL, normalized.c_str(), 0) != CURLUE_OK) return {};
  return part(url.get(), CURLUPART_HOST);
}

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t parse_date(std::string_view date) {
  // Atom / ISO-8601. Use chrono calendar validation and explicitly apply the UTC offset.
  if (date.size() >= 19 && date[4] == '-' && date[7] == '-' && (date[10] == 'T' || date[10] == ' ')) {
    auto number = [&](std::size_t start, std::size_t length) -> int {
      if (start + length > date.size()) return -1;
      int value = 0;
      for (auto ch : date.substr(start, length)) { if (ch < '0' || ch > '9') return -1; value = value * 10 + ch - '0'; }
      return value;
    };
    int y = number(0, 4), m = number(5, 2), d = number(8, 2);
    int h = number(11, 2), min = number(14, 2), sec = number(17, 2);
    const std::chrono::year_month_day day{std::chrono::year(y), std::chrono::month(static_cast<unsigned>(m)), std::chrono::day(static_cast<unsigned>(d))};
    if (!day.ok() || h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 59 || date[13] != ':' || date[16] != ':') return 0;
    std::size_t pos = 19;
    if (pos < date.size() && date[pos] == '.') { ++pos; while (pos < date.size() && date[pos] >= '0' && date[pos] <= '9') ++pos; }
    int offset = 0;
    if (pos < date.size() && (date[pos] == '+' || date[pos] == '-')) {
      int oh = number(pos + 1, 2), om = number(pos + 4, 2);
      if (oh < 0 || oh > 23 || om < 0 || om > 59 || pos + 6 != date.size() || date[pos + 3] != ':') return 0;
      offset = (oh * 3600 + om * 60) * (date[pos] == '+' ? 1 : -1);
    } else if (pos + 1 != date.size() || (date[pos] != 'Z' && date[pos] != 'z')) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::sys_days(day).time_since_epoch()).count() + h * 3600 + min * 60 + sec - offset;
  }
  auto value = curl_getdate(std::string(date).c_str(), nullptr);
  return value < 0 ? 0 : static_cast<std::int64_t>(value);
}

std::string display_date(std::int64_t date) {
  if (date <= 0) return "Unknown date";
  auto time = static_cast<std::time_t>(date);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out; out << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
  return out.str();
}

std::vector<Story> merge_stories(std::vector<Story> stories) {
  std::vector<Story> merged;
  std::unordered_map<std::string, std::size_t> positions;
  for (auto& story : stories) {
    story.url = normalize_url(story.url);
    if (story.url.empty()) continue;
    auto [it, added] = positions.emplace(story.url, merged.size());
    if (added) merged.push_back(std::move(story));
    else {
      auto& old = merged[it->second];
      old.read = old.read || story.read; old.bookmarked = old.bookmarked || story.bookmarked;
      old.published = std::max(old.published, story.published); old.seen = std::max(old.seen, story.seen);
      if (story.content.size() > old.content.size()) { old.content = std::move(story.content); old.kind = story.kind; }
    }
  }
  std::stable_sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
    return a.published != b.published ? a.published > b.published : a.url < b.url;
  });
  return merged;
}

std::vector<std::size_t> filter_stories(const std::vector<Story>& stories, const Filter& filter) {
  std::vector<std::size_t> result;
  auto key = search_key(filter.search);
  for (std::size_t i = 0; i < stories.size(); ++i) {
    const auto& s = stories[i];
    if (!filter.source.empty() && filter.source != s.source) continue;
    if (!filter.language.empty() && filter.language != s.language) continue;
    if (filter.unread_only && s.read) continue;
    if (filter.bookmarked_only && !s.bookmarked) continue;
    if (!key.empty() && search_key(s.title + " " + s.summary).find(key) == std::string::npos) continue;
    result.push_back(i);
  }
  return result;
}

bool technology_story(const Feed& feed, const std::string& url, const std::vector<std::string>& categories) {
  const auto host = url_host(url);
  if (feed.source == "Ars Technica" && host != "arstechnica.com" && host != "www.arstechnica.com") return false;
  if (feed.source == "TechCrunch" && host != "techcrunch.com" && host != "www.techcrunch.com") return false;
  if (feed.source == "Tecnoblog" && (host != "tecnoblog.net" || url.find("/noticias/") == std::string::npos)) return false;
  const std::vector<std::string> excluded = {
    "science", "space", "entertainment", "gaming", "games", "culture", "politics", "politica", "elections", "eleicoes",
    "justica eleitoral", "military", "ukraine", "war on", "invasion", "biotech", "health", "transportation",
    "cinema", "filmes", "series", "esportes", "streaming", "achados", "ofertas", "ciencia", "jogos"
  };
  for (const auto& category : categories) {
    auto value = search_key(category);
    for (const auto& word : excluded) if (value.find(word) != std::string::npos) return false;
  }
  if (feed.source == "Ars Technica")
    return url.find("/gadgets/") != std::string::npos || url.find("/information-technology/") != std::string::npos ||
           url.find("/security/") != std::string::npos || url.find("/ai/") != std::string::npos || url.find("/tech-policy/") != std::string::npos;
  return true; // The remaining curated feeds are already section-specific.
}
}  // namespace tech_news
