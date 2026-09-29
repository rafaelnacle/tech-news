#pragma once
#include "tech_news/http.hpp"
#include "tech_news/parser.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace tech_news {
struct WorkResult {
  enum class Type { Feed, RefreshDone, Article } type = Type::Feed;
  std::vector<Story> stories;
  ArticleText article;
  std::string url;
  std::string source;
  std::string error;
};

class Worker {
 public:
  using Fetch = std::function<HttpResponse(const std::string&, std::stop_token)>;
  explicit Worker(std::function<void()> wake, Fetch fetch = {}, std::vector<Feed> feeds = curated_feeds());
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  void refresh();
  bool article(const Story& story);
  std::vector<WorkResult> drain();
  void stop();
 private:
  struct Task { bool refresh = false; Story story; };
  void run(std::stop_token stop);
  void publish(WorkResult result);
  std::function<void()> wake_;
  Fetch fetch_;
  std::vector<Feed> feeds_;
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<Task> tasks_;
  std::vector<WorkResult> results_;
  bool refresh_pending_ = false;
  std::jthread thread_;
};
}  // namespace tech_news
