#include "tech_news/ui.hpp"
#include "tech_news/model.hpp"
#include "tech_news/platform.hpp"
#include "tech_news/store.hpp"
#include "tech_news/text.hpp"
#include "tech_news/worker.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace tech_news {
namespace {
using namespace ftxui;
class NewsUi {
 public:
  explicit NewsUi(bool offline)
      : offline_(offline), monochrome_(std::getenv("NO_COLOR") != nullptr), store_(data_path()),
        screen_(ScreenInteractive::Fullscreen()), worker_([this] { screen_.PostEvent(Event::Custom); }) {
    store_.prune(now_seconds()); stories_ = store_.load(); refreshed_at_ = store_.last_refresh();
    rebuild();
    InputOption option; option.multiline = false;
    input_ = Input(&filter_.search, "Search titles and summaries", option);
    screen_.TrackMouse(false);
    screen_.HandlePipedInput(false);
    screen_.ForceHandleCtrlC(false);
    status_ = offline_ ? "Offline - cached stories" : "Cached stories ready";
    if (!offline_) refresh();
  }
  int run() {
    auto component = Renderer(input_, [this] { return render(); });
    component = CatchEvent(component, [this](Event event) { return on_event(event); });
    screen_.Loop(component);
    worker_.stop();
    return 0;
  }
 private:
  static constexpr const char* sources_[4] = {"All sources", "Ars Technica", "TechCrunch", "Tecnoblog"};
  Story* selected() {
    if (visible_.empty()) return nullptr;
    return &stories_[visible_[static_cast<std::size_t>(selected_)]];
  }
  void rebuild(const std::string& preserve = {}) {
    visible_ = filter_stories(stories_, filter_);
    selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(visible_.size()) - 1));
    if (!preserve.empty())
      for (std::size_t i = 0; i < visible_.size(); ++i)
        if (stories_[visible_[i]].url == preserve) selected_ = static_cast<int>(i);
    auto* story = selected();
    if (!story || story->url != displayed_url_) { displayed_url_ = story ? story->url : ""; scroll_ = 0; reader_open_ = false; }
  }
  void refresh() {
    if (offline_) { status_ = "Offline mode: restart without --offline to refresh"; return; }
    if (refreshing_) { status_ = "Refresh already in progress"; return; }
    refreshing_ = true; source_errors_.clear(); successful_sources_ = 0;
    status_ = "Refreshing sources... cached stories remain available";
    worker_.refresh();
  }
  void apply_results() {
    for (auto& result : worker_.drain()) {
      try {
        auto* old = selected(); const auto url = old ? old->url : "";
        if (result.type == WorkResult::Type::Feed) {
          if (!result.error.empty()) { source_errors_.push_back(result.source + ": " + result.error); status_ = "Source failed; cached stories retained. Press ? for details."; }
          else { store_.upsert(result.stories); stories_ = store_.load(); ++successful_sources_; rebuild(url); }
        } else if (result.type == WorkResult::Type::RefreshDone) {
          refreshing_ = false;
          store_.prune(now_seconds()); stories_ = store_.load(); rebuild(url);
          if (successful_sources_) { refreshed_at_ = now_seconds(); store_.set_last_refresh(refreshed_at_); }
          status_ = source_errors_.empty() ? "Refresh complete" : "Stale cache - " + std::to_string(source_errors_.size()) + " source(s) failed. Press ? for details.";
        } else {
          loading_articles_.erase(result.url);
          if (!result.error.empty()) article_errors_[result.url] = result.error;
          else {
            store_.save_content(result.url, result.article.text, result.article.kind);
            for (auto& story : stories_) if (story.url == result.url) { story.content = std::move(result.article.text); story.kind = result.article.kind; }
            article_errors_.erase(result.url);
          }
        }
      } catch (const std::exception& error) { status_ = single_line(error.what()); }
    }
  }
  void read_article() {
    auto* story = selected(); if (!story) return;
    store_.set_flags(story->url, true, story->bookmarked); story->read = true;
    // Keep the just-opened story visible until returning to the list, even in the unread filter.
    reader_open_ = true; focus_ = 2; scroll_ = 0;
    if (story->kind == ContentKind::Feed || story->kind == ContentKind::Extracted || loading_articles_.contains(story->url)) return;
    if (offline_) { article_errors_[story->url] = "Offline: cached text or summary fallback. Press o to open browser when connected."; return; }
    if (worker_.article(*story)) { article_errors_.erase(story->url); loading_articles_.insert(story->url); }
    else status_ = "Article queue is full; press Enter again shortly";
  }
  void toggle_flag(bool bookmark) {
    auto* story = selected(); if (!story) return;
    auto url = story->url;
    const bool read = bookmark ? story->read : !story->read;
    const bool saved = bookmark ? !story->bookmarked : story->bookmarked;
    store_.set_flags(url, read, saved); story->read = read; story->bookmarked = saved;
    status_ = bookmark ? (saved ? "Bookmarked - retained until you remove the bookmark" : "Bookmark removed") : (read ? "Marked read" : "Marked unread");
    if (!reader_open_) rebuild(url);
  }
  void change_source() {
    source_index_ = (source_index_ + 1) % 4;
    filter_.source = source_index_ ? sources_[source_index_] : ""; rebuild();
  }
  void change_language() {
    language_index_ = (language_index_ + 1) % 3;
    filter_.language = language_index_ == 1 ? "en" : language_index_ == 2 ? "pt-BR" : ""; rebuild();
  }
  void activate_filter() {
    if (sidebar_row_ < 4) { source_index_ = sidebar_row_; filter_.source = source_index_ ? sources_[source_index_] : ""; }
    else if (sidebar_row_ < 7) { language_index_ = sidebar_row_ - 4; filter_.language = language_index_ == 1 ? "en" : language_index_ == 2 ? "pt-BR" : ""; }
    else if (sidebar_row_ == 7) filter_.unread_only = !filter_.unread_only;
    else if (sidebar_row_ == 8) filter_.bookmarked_only = !filter_.bookmarked_only;
    else { filter_ = {}; source_index_ = language_index_ = 0; }
    rebuild();
  }
  bool on_event(Event event) {
    apply_results();
    if (event == Event::Custom) return true;
    if (event == Event::CtrlC) { screen_.Exit(); return true; }
    if (searching_) {
      if (event == Event::Escape) { searching_ = false; filter_.search.clear(); rebuild(); return true; }
      if (event == Event::Return || event == Event::Tab) { searching_ = false; focus_ = 1; rebuild(); return true; }
      input_->OnEvent(event); filter_.search = single_line(filter_.search).substr(0, 200); rebuild(); return true;
    }
    if (event == Event::Character("q")) { screen_.Exit(); return true; }
    if (event == Event::Character("?")) { help_ = !help_; help_scroll_ = 0; return true; }
    if (help_) {
      if (event == Event::Escape || event == Event::Return) help_ = false;
      else if (event == Event::ArrowDown || event == Event::Character("j") || event == Event::PageDown)
        help_scroll_ = std::min(help_scroll_ + (event == Event::PageDown ? 10 : 1), std::max(0, help_box_.y_max - help_box_.y_min));
      else if (event == Event::ArrowUp || event == Event::Character("k") || event == Event::PageUp)
        help_scroll_ = std::max(0, help_scroll_ - (event == Event::PageUp ? 10 : 1));
      return true;
    }
    try {
      if (event == Event::Character("r")) refresh();
      else if (event == Event::Character("/")) { searching_ = true; input_->TakeFocus(); }
      else if (event == Event::Character("b")) toggle_flag(true);
      else if (event == Event::Character("u")) toggle_flag(false);
      else if (event == Event::Character("s")) change_source();
      else if (event == Event::Character("l")) change_language();
      else if (event == Event::Character("v")) { filter_.unread_only = !filter_.unread_only; rebuild(); }
      else if (event == Event::Character("m")) { filter_.bookmarked_only = !filter_.bookmarked_only; rebuild(); }
      else if (event == Event::Character("o")) { if (auto* story = selected()) { open_browser(story->url); status_ = "Opened article in browser"; } }
      else if (event == Event::Escape) { reader_open_ = false; focus_ = 1; scroll_ = 0; filter_.search.clear(); rebuild(); }
      else if (event == Event::Tab || event == Event::TabReverse) {
        const int direction = event == Event::Tab ? 1 : -1;
        if (width_ >= 110) focus_ = (focus_ + direction + 3) % 3;
        else if (width_ >= 80) focus_ = focus_ == 1 ? 2 : 1;
        else focus_ = reader_open_ ? 2 : 1;
      } else if (event == Event::Return) { if (focus_ == 0 && width_ >= 110) activate_filter(); else read_article(); }
      else if (event == Event::ArrowUp || event == Event::Character("k") || event == Event::ArrowDown || event == Event::Character("j") ||
               event == Event::PageUp || event == Event::PageDown || event == Event::Home || event == Event::End) {
        const bool up = event == Event::ArrowUp || event == Event::Character("k") || event == Event::PageUp || event == Event::Home;
        int amount = event == Event::PageUp || event == Event::PageDown ? std::max(1, height_ - 10) : 1;
        if (focus_ == 0 && width_ >= 110) sidebar_row_ = std::clamp(sidebar_row_ + (up ? -1 : 1), 0, 9);
        else if (focus_ == 2) {
          auto max = std::max(0, reader_box_.y_max - reader_box_.y_min);
          scroll_ = event == Event::Home ? 0 : event == Event::End ? max : std::clamp(scroll_ + (up ? -amount : amount), 0, max);
        } else {
          selected_ = event == Event::Home ? 0 : event == Event::End ? std::max(0, static_cast<int>(visible_.size()) - 1) :
            std::clamp(selected_ + (up ? -amount : amount), 0, std::max(0, static_cast<int>(visible_.size()) - 1));
          rebuild();
        }
      }
    } catch (const std::exception& error) { status_ = single_line(error.what()); }
    return true;
  }
  Element accent(Element element) const { return monochrome_ ? element : element | color(Color::RGB(83, 218, 235)); }
  Element muted(Element element) const { return monochrome_ ? element : element | color(Color::RGB(166, 180, 200)); }
  Element highlighted(Element element, bool focused) const {
    if (monochrome_) return focused ? element | inverted | bold : element | bold;
    return focused ? element | bgcolor(Color::RGB(83, 218, 235)) | color(Color::RGB(8, 17, 32)) | bold :
                     element | bgcolor(Color::RGB(30, 47, 68)) | bold;
  }
  Element panel(std::string title, Element body, bool focused) const {
    auto label = text(std::string(focused ? "> " : "  ") + title) | bold;
    return window(focused ? accent(label) : muted(label), body) | flex;
  }
  Element sidebar() const {
    std::vector<std::string> labels;
    for (int i = 0; i < 4; ++i) labels.push_back(std::string(source_index_ == i ? "[x] " : "[ ] ") + sources_[i]);
    labels.push_back(std::string(language_index_ == 0 ? "[x] " : "[ ] ") + "All languages");
    labels.push_back(std::string(language_index_ == 1 ? "[x] " : "[ ] ") + "English");
    labels.push_back(std::string(language_index_ == 2 ? "[x] " : "[ ] ") + "Portuguese (BR)");
    labels.push_back(std::string(filter_.unread_only ? "[x] " : "[ ] ") + "Unread only");
    labels.push_back(std::string(filter_.bookmarked_only ? "[x] " : "[ ] ") + "Bookmarked only");
    labels.push_back("Reset filters");
    Elements rows;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
      auto row = text(labels[static_cast<std::size_t>(i)]);
      if (focus_ == 0 && sidebar_row_ == i) row = highlighted(row, true) | focus;
      rows.push_back(row);
      if (i == 3 || i == 6) rows.push_back(separator());
    }
    rows.push_back(filler()); rows.push_back(muted(text("Enter applies filter")));
    return panel("Sources & filters", vbox(std::move(rows)) | vscroll_indicator | yframe | flex, focus_ == 0) | size(WIDTH, EQUAL, 25);
  }
  Element headlines() const {
    if (visible_.empty()) {
      const auto message = stories_.empty() ? (offline_ ? "No cached stories. Start without --offline to fetch news." : "No stories yet. Sources are loading; press r to retry after a failure.") :
                                             "No matching stories. Clear search with Esc or reset filters with s/l/v/m.";
      return panel("Headlines (0)", paragraph(message) | center, focus_ == 1);
    }
    Elements rows;
    for (std::size_t i = 0; i < visible_.size(); ++i) {
      const auto& story = stories_[visible_[i]];
      std::string markers = std::string(story.bookmarked ? "* " : "  ") + (story.read ? "  " : "+ ");
      auto row = vbox({text(markers + story.title), muted(text("    " + story.source + " / " + story.language + " / " + display_date(story.published).substr(0, 10)))});
      if (static_cast<int>(i) == selected_) { row = highlighted(row, focus_ == 1); row = row | focus; }
      rows.push_back(row); rows.push_back(text(""));
    }
    return panel("Headlines (" + std::to_string(visible_.size()) + ")  + unread  * saved", vbox(std::move(rows)) | vscroll_indicator | yframe | flex, focus_ == 1);
  }
  Element reader() {
    auto* story = selected();
    if (!story) return panel("Article preview", paragraph("Select a headline. Enter opens the reader; o opens the publisher in your browser.") | center, focus_ == 2);
    std::string label = "Summary preview - Enter reads / o browser";
    std::string body = story->summary;
    if (reader_open_) {
      if (!story->content.empty()) body = story->content;
      switch (story->kind) {
        case ContentKind::Feed: label = "Complete feed content"; break;
        case ContentKind::Extracted: label = "Public article text - completeness not verified"; break;
        case ContentKind::Incomplete: label = "Incomplete content - o opens full article in browser"; break;
        case ContentKind::Summary: label = "Summary fallback - o opens article in browser"; break;
      }
      if (loading_articles_.contains(story->url)) label = "Loading public article... summary available below";
      if (auto it = article_errors_.find(story->url); it != article_errors_.end()) label = it->second;
    }
    Elements blocks;
    blocks.push_back(paragraph(story->title) | bold);
    blocks.push_back(muted(paragraph(story->source + " / " + story->language + " / " + display_date(story->published))));
    blocks.push_back(separator()); blocks.push_back(accent(paragraph(label))); blocks.push_back(text(""));
    std::istringstream stream(body); std::string line;
    while (std::getline(stream, line)) {
      auto block = paragraph(line.starts_with("# ") ? line.substr(2) : line);
      if (line.starts_with("# ")) block = block | bold;
      blocks.push_back(line.empty() ? text("") : block);
    }
    blocks.push_back(text("")); blocks.push_back(muted(paragraph("o: Open publisher in browser   Esc: Back to headlines")));
    return panel(reader_open_ ? "Reader" : "Article preview", vbox(std::move(blocks)) | reflect(reader_box_) | focusPosition(0, scroll_) | vscroll_indicator | yframe | flex, focus_ == 2);
  }
  Element help_view() {
    Elements rows = {
      text("Keyboard help") | bold, separator(),
      paragraph("Arrows / j / k: move headlines, filters, or reader. Page Up / Down: move a page. Home / End: first / last."),
      paragraph("Tab / Shift+Tab: change pane focus. The focused pane has a > label. Enter: read article or apply sidebar filter. Esc: return / clear search."),
      paragraph("/: search titles and summaries locally. Enter keeps search; Esc clears. b: bookmark. u: toggle read status. + means unread; * means bookmarked."),
      paragraph("s: cycle source. l: cycle All / English / Portuguese. v: unread filter. m: bookmarked filter. These work when the sidebar is hidden."),
      paragraph("r: refresh. o: open public article in browser. ?: help. q: quit. NO_COLOR disables the palette."),
      paragraph("Full feed text is preferred. Public HTML extraction may be incomplete; restricted pages fall back to a summary. Bookmarks are retained; other stories expire after 30 days."),
      separator(), paragraph(status_),
    };
    if (source_errors_.empty()) rows.push_back(text("Source errors: none"));
    else for (const auto& error : source_errors_) rows.push_back(paragraph(error));
    rows.push_back(text("")); rows.push_back(accent(text("j/k scrolls help; Esc / ? closes help")));
    return window(accent(text("tech-news / Help")), vbox(std::move(rows)) | reflect(help_box_) | focusPosition(0, help_scroll_) | vscroll_indicator | yframe) | flex;
  }
  Element render() {
    auto dimensions = Terminal::Size(); width_ = dimensions.dimx; height_ = dimensions.dimy;
    if (width_ < 110 && focus_ == 0) focus_ = 1;
    if (width_ < 80) focus_ = reader_open_ ? 2 : 1;
    Element layout;
    if (width_ < 60 || height_ < 18) layout = vbox({text("tech-news") | bold, text("Resize terminal to at least 60 x 18"), text("q quits")}) | center;
    else if (help_) layout = help_view();
    else {
      std::string filters = std::string("s: ") + sources_[source_index_] + "  l: " + (language_index_ == 0 ? "All" : language_index_ == 1 ? "English" : "Portuguese") +
        "  v: " + (filter_.unread_only ? "Unread" : "All") + "  m: " + (filter_.bookmarked_only ? "Saved" : "All");
      Element content;
      if (width_ >= 110) content = hbox({sidebar(), headlines() | size(WIDTH, EQUAL, std::max(32, (width_ - 25) * 45 / 100)), reader()});
      else if (width_ >= 80) content = hbox({headlines() | size(WIDTH, EQUAL, width_ * 45 / 100), reader()});
      else content = reader_open_ ? reader() : headlines();
      auto search = searching_ ? hbox({accent(text("/ Search: ")), input_->Render() | flex}) :
        text(filter_.search.empty() ? "/ Search titles and summaries" : "/ Search: " + filter_.search);
      bool stale = !refreshed_at_ || now_seconds() - refreshed_at_ > 86400 || !source_errors_.empty();
      std::string cache = offline_ ? (stale ? "OFFLINE / STALE CACHE" : "OFFLINE") : refreshing_ ? "REFRESHING" : stale ? "STALE CACHE" : "CACHED";
      std::string timestamp = refreshed_at_ ? " / updated " + display_date(refreshed_at_) : " / no successful refresh yet";
      layout = vbox({hbox({accent(text(" tech-news ") | bold), muted(text(cache + timestamp))}),
        muted(text(filters)), search, content | flex, separator(), paragraph(status_),
        muted(text(width_ < 80 ? "Enter read  Esc back  b save  r refresh  ? help  q quit" :
          "j/k move  Tab focus  Enter read  / search  b save  u read  o browser  r refresh  ? help  q quit"))});
    }
    if (!monochrome_) layout = layout | color(Color::RGB(225, 232, 242)) | bgcolor(Color::RGB(10, 19, 35));
    return layout;
  }

  bool offline_;
  bool monochrome_;
  Store store_;
  ScreenInteractive screen_;
  Worker worker_; // Destroyed/joined before the screen (including exceptions).
  Component input_;
  std::vector<Story> stories_;
  std::vector<std::size_t> visible_;
  Filter filter_;
  std::set<std::string> loading_articles_;
  std::map<std::string, std::string> article_errors_;
  std::vector<std::string> source_errors_;
  std::string status_, displayed_url_;
  std::int64_t refreshed_at_ = 0;
  int source_index_ = 0, language_index_ = 0, selected_ = 0, sidebar_row_ = 0, focus_ = 1, scroll_ = 0;
  int width_ = 80, height_ = 24, successful_sources_ = 0, help_scroll_ = 0;
  Box reader_box_, help_box_;
  bool searching_ = false, help_ = false, reader_open_ = false, refreshing_ = false;
};
constexpr const char* NewsUi::sources_[4];
}  // namespace

int run_ui(bool offline) { return NewsUi(offline).run(); }
}  // namespace tech_news
