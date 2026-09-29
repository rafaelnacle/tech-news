#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "tech_news/http.hpp"
#include "tech_news/model.hpp"
#include "tech_news/parser.hpp"
#include "tech_news/store.hpp"
#include "tech_news/text.hpp"
#include "tech_news/worker.hpp"

using namespace tech_news;
using namespace std::chrono_literals;
namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename Function>
std::string throws(Function fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        return error.what();
    }
    throw std::runtime_error("Expected an error");
}
std::string fixture(const char* file) {
    std::ifstream input(std::filesystem::path(FIXTURE_DIR) / file);
    if (!input) throw std::runtime_error("Missing fixture");
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}
const Feed test_feed{"Fixture", "Fixture", "en", "https://news.example/rss"};
struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "tech-news-test-XXXXXX").string();
        auto* result = mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("Could not create test storage");
        path = result;
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
class Socket {
   public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) close(fd_);
    }
    int get() const { return fd_; }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

   private:
    int fd_;
};
class LocalServer {
   public:
    explicit LocalServer(std::string response, std::chrono::milliseconds delay = 0ms)
        : listener_(socket(AF_INET, SOCK_STREAM, 0)) {
        check(listener_.get() >= 0, "Could not create local test socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Could not bind local test server");
        check(listen(listener_.get(), 4) == 0, "Could not listen");
        socklen_t length = sizeof(address);
        check(getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0, "Could not inspect local port");
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/article";
        thread_ = std::jthread([this, response = std::move(response), delay](std::stop_token stop) {
            pollfd descriptor{listener_.get(), POLLIN, 0};
            while (!stop.stop_requested() && poll(&descriptor, 1, 50) <= 0) {
            }
            if (stop.stop_requested()) return;
            Socket client(accept(listener_.get(), nullptr, nullptr));
            if (client.get() < 0) return;
            accepted = true;
            char buffer[4096];
            pollfd client_descriptor{client.get(), POLLIN, 0};
            if (poll(&client_descriptor, 1, 1000) > 0) static_cast<void>(recv(client.get(), buffer, sizeof(buffer), 0));
            std::mutex mutex;
            std::unique_lock lock(mutex);
            std::condition_variable_any condition;
            condition.wait_for(lock, stop, delay, [] { return false; });
            if (stop.stop_requested()) return;
            std::size_t written = 0;
            while (written < response.size()) {
                auto size = send(client.get(), response.data() + written, response.size() - written, 0);
                if (size <= 0) break;
                written += static_cast<std::size_t>(size);
            }
        });
    }
    std::string url;
    std::atomic<bool> accepted{false};

   private:
    Socket listener_;
    std::jthread thread_;
};
std::string response(const std::string& body, const std::string& extra = {}) {
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

void test_text() {
    check(sanitize_text("safe\033[31m red\033[0m\033]52;c;payload\a text\a\r\nend") == "safe red text\nend", "ANSI/OSC sanitization");
    check(sanitize_text("a\033Pmalicious\033\\b") == "ab", "DCS sanitization");
    check(sanitize_text("a\xc2\x9b"
                        "31mb\xc2\x9d"
                        "evil\xc2\x9c"
                        "c") == "abc",
          "C1 terminal controls");
    check(sanitize_text("x\xe2\x80\xae"
                        "y") == "xy",
          "Bidi override sanitization");
    check(sanitize_text(std::string("\xff", 1)) == "\xef\xbf\xbd", "Invalid UTF-8 repaired");
    check(single_line("  Segurança\n e\tproteção  ") == "Segurança e proteção", "Whitespace and Portuguese");
    check(search_key("INTELIGÊNCIA, Proteção e AÇÃO") == "inteligencia, protecao e acao", "Accent-insensitive local search");
    check(normalize_url("https://EXAMPLE.com:443/a/?utm_source=rss&b=2&a=1#part") == "https://example.com/a?a=1&b=2", "URL normalization");
    check(normalize_url("file:///tmp/a").empty() && normalize_url("javascript:alert(1)").empty(), "URL protocols");
    check(normalize_url("https://user:password@example.com/a").empty(), "URL credentials");
    check(normalize_url("https://example.com/a\n").empty(), "URL control characters");
    check(resolve_url("https://example.com/feed/rss", "../story") == "https://example.com/story", "Relative article links");
    check(parse_date("Mon, 28 Sep 2026 14:00:00 GMT") == parse_date("2026-09-28T11:00:00.123-03:00"), "RSS/Atom offsets");
    check(parse_date("2026-09-28T14:00:00Z") == 1790604000, "Known UTC timestamp");
    check(parse_date("2026-02-30T14:00:00Z") == 0 && parse_date("nonsense") == 0, "Invalid dates");
    check(parse_date("2026-09-28T14:00:00+25:00") == 0 && parse_date("2026-09-28T14:00:00") == 0, "Invalid timezone");
}
void test_feeds() {
    const auto now = parse_date("2026-09-29T00:00:00Z");
    auto stories = parse_feed(fixture("rss.xml"), test_feed, now);
    check(stories.size() == 3, "RSS deduplication and exclusion");
    check(stories[0].title == "Software & security update" && stories[0].url == "https://news.example/update?id=7", "RSS entities and canonical URL");
    check(stories[0].summary == "A safe software summary." && stories[0].kind == ContentKind::Incomplete, "HTML summary and excerpt labeling");
    check(stories[1].published == parse_date("2026-09-27T13:30:00Z") && stories[2].published == 0, "Newest first and unknown dates last");
    auto atom_feed = test_feed;
    atom_feed.language = "pt-BR";
    auto atom = parse_feed(fixture("atom.xml"), atom_feed, now);
    check(atom.size() == 1 && atom[0].language == "pt-BR", "Atom Portuguese language");
    check(atom[0].kind == ContentKind::Feed && atom[0].content.find("# Atualização do sistema") != std::string::npos &&
              atom[0].content.find("- Segurança na leitura") != std::string::npos,
          "Complete XHTML paragraphs/headings/lists");
    check(atom[0].url == "https://news.example/ia", "Atom alternate link");
    check(!throws([&] { parse_feed(fixture("malformed.xml"), test_feed, now); }).empty(), "Malformed feed error");
    check(!throws([&] { parse_feed(fixture("external-entity.xml"), test_feed, now); }).empty(), "External entity rejection");
    check(!throws([&] { parse_feed("<html><body>error</body></html>", test_feed, now); }).empty(), "Non-feed error");
    check(!throws([&] { parse_feed(std::string(2 * 1024 * 1024 + 1, 'x'), test_feed, now); }).empty(), "Feed size bound");
    auto ars = curated_feeds()[0];
    check(technology_story(ars, "https://arstechnica.com/security/2026/example", {"Security"}), "Technology category included");
    check(!technology_story(ars, "https://arstechnica.com/science/2026/example", {"Science"}), "Science excluded");
    check(!technology_story(ars, "https://arstechnica.com/gadgets/2026/example", {"military robots", "Ukraine war"}), "Unrelated military excluded");
    check(!technology_story(curated_feeds().back(), "https://tecnoblog.net/achados/deal", {"Hardware"}), "Shopping excluded");
    check(!technology_story(curated_feeds().back(), "https://tecnoblog.net/noticias/election", {"Eleições 2026"}), "General politics excluded");
    check(!technology_story(ars, "https://untrusted.example/security/example", {"Security"}), "Publisher link identity");
    auto excerpt_xml = std::string("<rss xmlns:content='http://purl.org/rss/1.0/modules/content/'><channel><item><title>Long excerpt</title><link>https://news.example/excerpt</link><content:encoded><![CDATA[") +
                       "<p>" + std::string(1000, 'a') + "</p><p>Another excerpt paragraph.</p><p>Read full article</p>]]></content:encoded></item></channel></rss>";
    check(parse_feed(excerpt_xml, test_feed, now)[0].kind == ContentKind::Incomplete, "Long Ars-style excerpt still requires public HTML fetch");
}
void test_filter_merge() {
    auto stories = parse_feed(fixture("rss.xml"), test_feed, parse_date("2026-09-29T00:00:00Z"));
    auto atom_feed = test_feed;
    atom_feed.language = "pt-BR";
    atom_feed.source = "Portuguese fixture";
    auto portuguese = parse_feed(fixture("atom.xml"), atom_feed, now_seconds());
    stories.insert(stories.end(), portuguese.begin(), portuguese.end());
    stories[0].read = true;
    stories[0].bookmarked = true;
    auto duplicate = stories[0];
    duplicate.read = false;
    duplicate.bookmarked = false;
    duplicate.url += "&utm_source=other";
    stories.push_back(duplicate);
    stories = merge_stories(std::move(stories));
    check(stories.size() == 4, "Duplicate tracking URL removed");
    Filter filter;
    filter.search = "inteligencia";
    auto results = filter_stories(stories, filter);
    check(results.size() == 1 && stories[results[0]].language == "pt-BR", "Portuguese title search");
    filter.search = "protecao";
    check(filter_stories(stories, filter).size() == 1, "Summary search");
    filter = {};
    filter.source = "Fixture";
    check(filter_stories(stories, filter).size() == 3, "Source filter");
    filter = {};
    filter.language = "pt-BR";
    check(filter_stories(stories, filter).size() == 1, "Language filter");
    filter = {};
    filter.unread_only = true;
    check(filter_stories(stories, filter).size() == 3, "Unread filter");
    filter.bookmarked_only = true;
    check(filter_stories(stories, filter).empty(), "Combined filters");
    filter.unread_only = false;
    check(filter_stories(stories, filter).size() == 1, "Bookmark filter and preserved flags");
}
void test_extraction() {
    for (const auto& url : {"https://arstechnica.com/gadgets/story", "https://techcrunch.com/story", "https://tecnoblog.net/noticias/story", "https://generic.example/story"}) {
        auto article = extract_article(fixture("article.html"), url);
        check(article.kind == ContentKind::Extracted, "Public article extraction");
        check(article.text.find("# A careful software release") != std::string::npos && article.text.find("\n\n") != std::string::npos, "Headings/paragraphs preserved");
        check(article.text.find("- Second useful point.") != std::string::npos, "Lists and inline markup preserved");
        check(article.text.find("UNWANTED") == std::string::npos, "Navigation/ad/script/style removed");
    }
    auto restricted = extract_article(fixture("restricted.html"), "https://arstechnica.com/story");
    check(restricted.kind == ContentKind::Incomplete, "Restricted excerpt labeled incomplete");
    check(extract_article("<html><body><nav>Nothing readable</nav></body></html>", "https://generic.example").text.empty(), "Summary fallback");
    check(html_to_text("<p>notebook<em>s</em>.</p><p>Olá &amp; adeus!</p>") == "notebooks.\n\nOlá & adeus!", "Inline punctuation and entities");
}
void test_store() {
    TempDirectory directory;
    auto file = directory.path / "news.sqlite3";
    const auto now = now_seconds();
    Story saved{"https://news.example/saved", "Saved", "Summary", "Full cached article", "Fixture", "en", now - 60 * 86400, now - 60 * 86400, ContentKind::Extracted};
    Story expired = saved;
    expired.url = "https://news.example/expired";
    Story current = saved;
    current.url = "https://news.example/current";
    current.published = now;
    current.seen = now;
    Story unknown = saved;
    unknown.url = "https://news.example/unknown-date";
    unknown.published = 0;
    {
        Store store(file);
        store.upsert({saved, expired, current, unknown});
        store.set_flags(saved.url, true, true);
        unknown.seen = now;
        store.upsert({unknown});
        auto updated = saved;
        updated.summary = "Updated summary";
        updated.content = "Short excerpt";
        updated.kind = ContentKind::Incomplete;
        store.upsert({updated});
        store.set_last_refresh(now);
        store.prune(now);
    }
    {
        Store store(file);
        auto loaded = store.load();
        check(loaded.size() == 2, "30 day prune and bookmark retention");
        auto it = std::find_if(loaded.begin(), loaded.end(), [&](const auto& story) { return story.url == saved.url; });
        check(it != loaded.end() && it->read && it->bookmarked && it->content == saved.content && it->summary == "Updated summary", "Restart persistence and refresh preservation");
        check(store.last_refresh() == now, "Refresh metadata persistence");
        store.save_content(current.url, "Fetched text\033[31m", ContentKind::Extracted);
        auto again = store.load();
        check(again[0].content == "Fetched text", "Fetched text persisted and sanitized");
        store.set_flags(saved.url, true, false);
        store.prune(now);
        check(store.load().size() == 1, "Unbookmarked old story expires");
    }
    check((std::filesystem::status(file).permissions() & std::filesystem::perms::group_all) == std::filesystem::perms::none, "Private database permissions");
    check(!throws([&] { Store missing(directory.path / "missing" / "news.sqlite3"); }).empty(), "Persistence failures explicit");
}
void test_http() {
    {
        LocalServer server(response("hello"));
        auto result = HttpClient{}.get(server.url);
        check(result.body == "hello" && result.content_type.starts_with("text/html"), "HTTP response");
    }
    {
        LocalServer server("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        auto error = throws([&] { HttpClient{}.get(server.url + "?token=private-value"); });
        check(error.find("503") != std::string::npos && error.find("private-value") == std::string::npos, "HTTP failure and safe errors");
    }
    {
        LocalServer server(response(std::string(8000, 'x')));
        HttpOptions options;
        options.max_bytes = 1024;
        check(throws([&] { HttpClient(options).get(server.url); }).find("size limit") != std::string::npos, "Response-size limit");
    }
    {
        LocalServer server("HTTP/1.1 302 Found\r\nLocation: file:///etc/passwd\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        check(!throws([&] { HttpClient{}.get(server.url); }).empty(), "Redirect protocol restriction");
    }
    {
        LocalServer server(response("late"), 400ms);
        HttpOptions options;
        options.total_timeout_ms = 60;
        auto start = std::chrono::steady_clock::now();
        check(throws([&] { HttpClient(options).get(server.url); }).find("Timeout") != std::string::npos, "Bounded timeout");
        check(std::chrono::steady_clock::now() - start < 1s, "Timeout duration");
    }
    {
        LocalServer server(response("late"), 5s);
        std::atomic<bool> cancelled{false};
        auto start = std::chrono::steady_clock::now();
        std::jthread request([&](std::stop_token stop) { cancelled = throws([&] { HttpClient{}.get(server.url, stop); }).find("cancelled") != std::string::npos; });
        for (int i = 0; i < 100 && !server.accepted; ++i) std::this_thread::sleep_for(5ms);
        request.request_stop();
        request.join();
        check(cancelled && std::chrono::steady_clock::now() - start < 2s, "In-flight cancellation");
    }
    check(!throws([&] { HttpClient{}.get("file:///etc/passwd"); }).empty(), "Non-HTTP request rejected");
    check(!throws([&] { HttpClient{}.get("https://user:password@example.com"); }).empty(), "Request credentials rejected");
    std::stop_source stopped;
    stopped.request_stop();
    check(throws([&] { HttpClient{}.get("https://news.example", stopped.get_token()); }) == "Request cancelled", "Pre-cancelled request");
}
void test_worker() {
    std::mutex mutex;
    std::condition_variable condition;
    int wakes = 0;
    std::atomic<int> requests{0};
    std::vector<Feed> feeds = {test_feed, {"Malformed", "Fixture", "en", "https://news.example/broken"}, {"Failed", "Fixture", "en", "https://news.example/fail"}};
    Worker worker([&] { std::lock_guard lock(mutex); ++wakes; condition.notify_all(); },
                  [&](const std::string& url, std::stop_token) -> HttpResponse {
                      ++requests;
                      if (url.ends_with("broken")) return {fixture("malformed.xml"), "text/xml"};
                      if (url.ends_with("fail")) throw std::runtime_error("Fixture source unavailable");
                      return {url.ends_with("rss") ? fixture("rss.xml") : fixture("article.html"), "text/xml"};
                  },
                  feeds);
    worker.refresh();
    worker.refresh();
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(lock, 3s, [&] { return wakes >= 4; }), "Worker refresh completion");
    }
    auto results = worker.drain();
    check(results.size() == 4 && requests == 3, "Refresh coalescing");
    check(results[0].stories.size() == 3 && !results[1].error.empty() && !results[2].error.empty() && results[3].type == WorkResult::Type::RefreshDone, "Partial source failures and malformed responses");
    TempDirectory directory;
    Store store(directory.path / "news.sqlite3");
    store.upsert(results[0].stories);
    store.set_flags(results[0].stories[0].url, true, true);
    for (const auto& result : results)
        if (result.error.empty()) store.upsert(result.stories);
    check(store.load().size() == 3 && store.load()[0].bookmarked, "Usable cache retained during source failures");
    check(worker.article(results[0].stories[0]), "Article task accepted");
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(lock, 3s, [&] { return wakes >= 5; }), "Worker article completion");
    }
    auto article = worker.drain();
    check(article.size() == 1 && article[0].article.kind == ContentKind::Extracted, "Worker extracts article off UI thread");
    worker.stop();
    std::atomic<bool> started{false}, cancelled{false};
    Worker cancelling([] {}, [&](const std::string&, std::stop_token stop) -> HttpResponse {
    started = true; std::mutex wait_mutex; std::unique_lock lock(wait_mutex); std::condition_variable_any wait;
    wait.wait_for(lock, stop, 5s, [] { return false; }); cancelled = stop.stop_requested(); throw std::runtime_error("Cancelled"); }, {test_feed});
    cancelling.refresh();
    for (int i = 0; i < 100 && !started; ++i) std::this_thread::sleep_for(5ms);
    auto start = std::chrono::steady_clock::now();
    cancelling.stop();
    check(cancelled && std::chrono::steady_clock::now() - start < 1s, "Worker cancels and joins on exit");
    check(cancelling.drain().empty(), "No stale results published after cancellation");
}
}  // namespace
int main() {
    std::signal(SIGPIPE, SIG_IGN);
    HttpRuntime runtime;
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"text / URL / dates", test_text},
        {"RSS / Atom / scope", test_feeds},
        {"merge / filters", test_filter_merge},
        {"article extraction / fallback", test_extraction},
        {"SQLite persistence / retention", test_store},
        {"HTTP limits / failures / cancellation", test_http},
        {"background work / partial failures", test_worker},
    };
    int failed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
