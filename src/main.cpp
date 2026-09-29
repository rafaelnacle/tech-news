#include "tech_news/http.hpp"
#include "tech_news/text.hpp"
#include "tech_news/ui.hpp"
#include <libxml/parser.h>
#include <array>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t termination_signal = 0;
void record_termination(int signal) { termination_signal = signal; }
// FTXUI restores and re-raises termination signals after restoring the terminal.
// Record that re-raised signal so Loop can return and the worker can join first.
class TerminationSignals {
 public:
  TerminationSignals() {
    struct sigaction handler{};
    handler.sa_handler = record_termination;
    sigemptyset(&handler.sa_mask);
    for (std::size_t i = 0; i < signals_.size(); ++i) {
      if (sigaction(signals_[i], &handler, &previous_[i]) != 0) {
        for (std::size_t j = 0; j < installed_; ++j) sigaction(signals_[j], &previous_[j], nullptr);
        throw std::runtime_error("Could not configure termination cleanup");
      }
      ++installed_;
    }
  }
  ~TerminationSignals() { for (std::size_t i = 0; i < installed_; ++i) sigaction(signals_[i], &previous_[i], nullptr); }
  TerminationSignals(const TerminationSignals&) = delete;
  TerminationSignals& operator=(const TerminationSignals&) = delete;
 private:
  const std::array<int, 4> signals_{SIGINT, SIGTERM, SIGHUP, SIGQUIT};
  std::array<struct sigaction, 4> previous_{};
  std::size_t installed_ = 0;
};
constexpr std::string_view help =
  "tech-news — technology news in your terminal\n\n"
  "Usage: tech-news [--offline] [--help] [--version]\n\n"
  "  --offline  Read the local cache without network requests\n"
  "  --help     Show this help\n"
  "  --version  Show the version\n\n"
  "Controls: arrows/j/k move, Tab changes focus, Enter reads, Esc returns,\n"
  "          / searches, b bookmarks, u toggles read, o opens browser,\n"
  "          r refreshes, s source, l language, v unread filter,\n"
  "          m bookmark filter, ? help, q quits.\n"
  "Storage: Linux $XDG_DATA_HOME/tech-news (default ~/.local/share/tech-news);\n"
  "         macOS ~/Library/Application Support/tech-news.\n"
  "Set NO_COLOR for monochrome. No API keys or .env are required.\n";
}
int main(int argc, char** argv) {
  bool offline = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") { std::cout << help; return 0; }
    if (arg == "--version") { std::cout << "tech-news " << TECH_NEWS_VERSION << '\n'; return 0; }
    if (arg == "--offline") offline = true;
    else { std::cerr << "Unknown option. Run tech-news --help.\n"; return 2; }
  }
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    std::cerr << "tech-news needs an interactive terminal. Run tech-news --help for options.\n"; return 2;
  }
  try {
    TerminationSignals signals;
    tech_news::HttpRuntime http;
    xmlInitParser();
    // Never let XML/HTML resolve external documents, even if parser options change later.
    xmlSetExternalEntityLoader([](const char*, const char*, xmlParserCtxtPtr) -> xmlParserInputPtr { return nullptr; });
    const auto code = tech_news::run_ui(offline);
    xmlCleanupParser();
    return termination_signal ? 128 + termination_signal : code;
  } catch (const std::exception& error) {
    std::cerr << "tech-news: " << tech_news::single_line(error.what()) << '\n'; return 1;
  }
}
