#include "tech_news/http.hpp"

#include <curl/curl.h>

#include <memory>
#include <stdexcept>

#include "tech_news/text.hpp"

namespace tech_news {
HttpRuntime::HttpRuntime() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Could not initialize HTTP client");
}
HttpRuntime::~HttpRuntime() { curl_global_cleanup(); }
namespace {
struct Transfer {
    std::string body;
    std::size_t limit;
    std::stop_token stop;
    bool too_large = false;
    bool allocation_failed = false;
};
std::size_t write_data(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& state = *static_cast<Transfer*>(context);
    if (state.stop.stop_requested()) return 0;
    if (size && count > state.limit / size) {
        state.too_large = true;
        return 0;
    }
    const auto bytes = size * count;
    if (bytes > state.limit - state.body.size()) {
        state.too_large = true;
        return 0;
    }
    try {
        state.body.append(data, bytes);
    } catch (...) {
        state.allocation_failed = true;
        return 0;
    }
    return bytes;
}
int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<Transfer*>(context)->stop.stop_requested() ? 1 : 0;
}
}  // namespace

HttpResponse HttpClient::get(const std::string& input, std::stop_token stop) const {
    const auto url = normalize_url(input);
    if (url.empty()) throw std::runtime_error("Only HTTP(S) URLs without credentials are allowed");
    if (stop.stop_requested()) throw std::runtime_error("Request cancelled");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Could not create HTTP request");
    Transfer state{{}, options_.max_bytes, stop};
    auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(handle.get(), option, value) != CURLE_OK) throw std::runtime_error("Could not configure HTTP request");
    };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 1L);
    set(CURLOPT_MAXREDIRS, 4L);
    set(CURLOPT_CONNECTTIMEOUT_MS, options_.connect_timeout_ms);
    set(CURLOPT_TIMEOUT_MS, options_.total_timeout_ms);
    set(CURLOPT_LOW_SPEED_LIMIT, 50L);
    set(CURLOPT_LOW_SPEED_TIME, 8L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_UNRESTRICTED_AUTH, 0L);
    set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    set(CURLOPT_USERAGENT, "tech-news/1.0 (+terminal RSS reader)");
    set(CURLOPT_ACCEPT_ENCODING, "");
    set(CURLOPT_WRITEFUNCTION, write_data);
    set(CURLOPT_WRITEDATA, &state);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, progress);
    set(CURLOPT_XFERINFODATA, &state);
    const auto code = curl_easy_perform(handle.get());
    if (stop.stop_requested()) throw std::runtime_error("Request cancelled");
    if (state.too_large) throw std::runtime_error("Response exceeds download size limit");
    if (state.allocation_failed) throw std::runtime_error("Not enough memory for response");
    if (code != CURLE_OK) throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(code));
    long status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw std::runtime_error("Publisher returned HTTP " + std::to_string(status));
    char* type = nullptr;
    curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_TYPE, &type);
    return {std::move(state.body), type ? single_line(type) : ""};
}
}  // namespace tech_news
