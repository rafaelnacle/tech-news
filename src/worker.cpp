#include "tech_news/worker.hpp"
#include "tech_news/text.hpp"
#include <algorithm>
#include <stdexcept>

namespace tech_news {
Worker::Worker(std::function<void()> wake, Fetch fetch, std::vector<Feed> feeds)
    : wake_(std::move(wake)), fetch_(std::move(fetch)), feeds_(std::move(feeds)) {
  if (!fetch_) fetch_ = [](const auto& url, auto stop) { return HttpClient{}.get(url, stop); };
  thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}
Worker::~Worker() { stop(); }
void Worker::stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop(); condition_.notify_all(); thread_.join();
}
void Worker::refresh() {
  std::lock_guard lock(mutex_);
  if (refresh_pending_) return;
  refresh_pending_ = true; tasks_.push_back({true, {}}); condition_.notify_one();
}
bool Worker::article(const Story& story) {
  std::lock_guard lock(mutex_);
  if (tasks_.size() >= 8) return false;
  if (std::any_of(tasks_.begin(), tasks_.end(), [&](const auto& task) { return !task.refresh && task.story.url == story.url; })) return true;
  tasks_.push_front({false, story}); condition_.notify_one();
  return true;
}
std::vector<WorkResult> Worker::drain() {
  std::lock_guard lock(mutex_);
  auto result = std::move(results_); results_.clear(); return result;
}
void Worker::publish(WorkResult result) {
  { std::lock_guard lock(mutex_); results_.push_back(std::move(result)); }
  wake_();
}
void Worker::run(std::stop_token stop) {
  auto read_article = [&](const Story& story) {
    WorkResult result; result.type = WorkResult::Type::Article; result.url = story.url;
    try {
      auto response = fetch_(story.url, stop);
      result.article = extract_article(response.body, story.url);
      if (result.article.text.empty()) result.error = "Public article text unavailable. Summary fallback; press o to open browser.";
    } catch (const std::exception& error) { result.error = "Summary fallback: " + single_line(error.what()) + ". Press o to open browser."; }
    if (!stop.stop_requested()) publish(std::move(result));
  };
  while (!stop.stop_requested()) {
    Task task;
    {
      std::unique_lock lock(mutex_);
      if (!condition_.wait(lock, stop, [&] { return !tasks_.empty(); })) break;
      task = std::move(tasks_.front()); tasks_.pop_front();
    }
    if (!task.refresh) { read_article(task.story); continue; }
    for (const auto& feed : feeds_) {
      if (stop.stop_requested()) break;
      WorkResult result; result.source = feed.name;
      try { result.stories = parse_feed(fetch_(feed.url, stop).body, feed, now_seconds()); }
      catch (const std::exception& error) { result.error = single_line(error.what()); }
      if (!stop.stop_requested()) publish(std::move(result));
      // A reader request gets priority between bounded source requests.
      while (!stop.stop_requested()) {
        Task reader;
        {
          std::lock_guard lock(mutex_);
          if (tasks_.empty() || tasks_.front().refresh) break;
          reader = std::move(tasks_.front()); tasks_.pop_front();
        }
        read_article(reader.story);
      }
    }
    { std::lock_guard lock(mutex_); refresh_pending_ = false; }
    if (!stop.stop_requested()) { WorkResult result; result.type = WorkResult::Type::RefreshDone; publish(std::move(result)); }
  }
}
}  // namespace tech_news
