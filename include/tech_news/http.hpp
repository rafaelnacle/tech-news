#pragma once
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>

namespace tech_news {
class HttpRuntime {
 public:
  HttpRuntime();
  ~HttpRuntime();
  HttpRuntime(const HttpRuntime&) = delete;
  HttpRuntime& operator=(const HttpRuntime&) = delete;
};

struct HttpOptions {
  long connect_timeout_ms = 5000;
  long total_timeout_ms = 20000;
  std::size_t max_bytes = 4 * 1024 * 1024;
};
struct HttpResponse { std::string body; std::string content_type; };
class HttpClient {
 public:
  explicit HttpClient(HttpOptions options = {}) : options_(options) {}
  HttpResponse get(const std::string& url, std::stop_token stop = {}) const;
 private:
  HttpOptions options_;
};
}  // namespace tech_news
