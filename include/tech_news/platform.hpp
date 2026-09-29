#pragma once
#include <filesystem>
#include <string>

namespace tech_news {
std::filesystem::path data_path();
void open_browser(const std::string& url);
}  // namespace tech_news
