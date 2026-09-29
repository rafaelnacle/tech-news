#include "tech_news/parser.hpp"

#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>

#include "tech_news/text.hpp"

namespace tech_news {
namespace {
using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
using XmlString = std::unique_ptr<xmlChar, decltype(xmlFree)>;
using XPathContext = std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)>;
using XPathResult = std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)>;
std::string name(const xmlNode* node) {
    return node && node->name ? reinterpret_cast<const char*>(node->name) : "";
}
std::string content(xmlNode* node) {
    if (!node) return {};
    XmlString value(xmlNodeGetContent(node), xmlFree);
    return value ? reinterpret_cast<const char*>(value.get()) : "";
}
std::string attribute(xmlNode* node, const char* key) {
    XmlString value(xmlGetProp(node, BAD_CAST key), xmlFree);
    return value ? reinterpret_cast<const char*>(value.get()) : "";
}
xmlNode* child(xmlNode* parent, const std::string& key) {
    if (!parent) return nullptr;
    for (auto* node = parent->children; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && name(node) == key) return node;
    return nullptr;
}
std::string markup(xmlNode* node) {
    if (!node) return {};
    bool elements = false;
    for (auto* current = node->children; current; current = current->next)
        elements = elements || current->type == XML_ELEMENT_NODE;
    if (!elements) return content(node);
    std::unique_ptr<xmlBuffer, decltype(&xmlBufferFree)> buffer(xmlBufferCreate(), xmlBufferFree);
    if (!buffer) throw std::runtime_error("Could not allocate parser buffer");
    for (auto* current = node->children; current; current = current->next)
        xmlNodeDump(buffer.get(), node->doc, current, 0, 0);
    return reinterpret_cast<const char*>(xmlBufferContent(buffer.get()));
}
bool noise(xmlNode* node) {
    const auto tag = name(node);
    if (tag == "script" || tag == "style" || tag == "nav" || tag == "aside" || tag == "footer" ||
        tag == "header" || tag == "form" || tag == "button" || tag == "iframe" || tag == "noscript" ||
        tag == "svg" || tag == "figure" || tag == "img" || tag == "video" || tag == "audio") return true;
    if (attribute(node, "aria-hidden") == "true" || xmlHasProp(node, BAD_CAST "hidden")) return true;
    auto tokens = search_key(attribute(node, "class") + " " + attribute(node, "id"));
    for (const auto& bad : {"advert", "advertisement", "ad-container", "ad-slot", "adsbygoogle", "related-post", "related-article",
                            "newsletter", "social-share", "share-buttons", "cookie", "comments", "author-bio", "post-tags", "wp-block-buttons"})
        if (tokens.find(bad) != std::string::npos) return true;
    return tokens == "ad" || tokens == "ads";
}
bool block_tag(const std::string& tag) {
    return tag == "p" || tag == "div" || tag == "section" || tag == "article" || tag == "main" ||
           tag == "blockquote" || tag == "ul" || tag == "ol" || tag == "li" || tag == "pre" ||
           (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6');
}
void line_break(std::string& out) {
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return;
    if (out.back() != '\n') out += '\n';
    if (out.size() < 2 || out[out.size() - 2] != '\n') out += '\n';
}
void walk(xmlNode* node, std::string& out, int depth = 0) {
    if (depth > 64 || out.size() > 250000) return;
    for (auto* current = node; current && out.size() <= 250000; current = current->next) {
        if (current->type == XML_TEXT_NODE || current->type == XML_CDATA_SECTION_NODE) {
            auto value = sanitize_text(content(current));
            for (char ch : value) {
                if (ch == ' ' || ch == '\n') {
                    if (!out.empty() && out.back() != '\n' && out.back() != ' ') out += ' ';
                } else
                    out += ch;
            }
        } else if (current->type == XML_ELEMENT_NODE && !noise(current)) {
            auto tag = name(current);
            bool block = block_tag(tag);
            if (block || tag == "br") line_break(out);
            if (tag == "li")
                out += "- ";
            else if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6')
                out += "# ";
            walk(current->children, out, depth + 1);
            if (block) line_break(out);
        }
    }
}
std::string node_text(xmlNode* node) {
    std::string text;
    if (node) walk(node->children, text);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return sanitize_text(text);
}
Document html_doc(std::string_view html) {
    if (html.size() > 4 * 1024 * 1024) throw std::runtime_error("HTML exceeds size limit");
    return Document(htmlReadMemory(html.data(), static_cast<int>(html.size()), nullptr, "UTF-8",
                                   HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT),
                    xmlFreeDoc);
}
bool incomplete(std::string_view text) {
    auto value = search_key(text);
    for (const auto& marker : {"continue reading", "read more", "read full article", "leia mais", "subscribe to read", "subscriber-only", "assine para", "exclusive to subscribers"})
        if (value.find(marker) != std::string::npos) return true;
    return false;
}
std::string select_text(xmlDoc* doc, const char* query) {
    XPathContext context(xmlXPathNewContext(doc), xmlXPathFreeContext);
    if (!context) throw std::runtime_error("Could not allocate HTML selector");
    XPathResult result(xmlXPathEvalExpression(BAD_CAST query, context.get()), xmlXPathFreeObject);
    std::string best;
    if (result && result->nodesetval)
        for (int i = 0; i < result->nodesetval->nodeNr; ++i) {
            auto text = node_text(result->nodesetval->nodeTab[i]);
            if (text.size() > best.size()) best = std::move(text);
        }
    return best;
}
}  // namespace

std::string html_to_text(std::string_view html) {
    if (html.empty()) return {};
    auto doc = html_doc(html);
    if (!doc) return single_line(html);
    return node_text(xmlDocGetRootElement(doc.get()));
}

std::vector<Story> parse_feed(std::string_view xml, const Feed& feed, std::int64_t now) {
    if (xml.empty() || xml.size() > 2 * 1024 * 1024) throw std::runtime_error("Feed is empty or exceeds size limit");
    // DTDs are unnecessary for RSS/Atom. Reject them rather than permitting any entity loading.
    std::string prefix(xml);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (prefix.find("<!doctype") != std::string::npos || prefix.find("<!entity") != std::string::npos)
        throw std::runtime_error("Feed contains a prohibited DTD or entity declaration");
    Document doc(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
                               XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_COMPACT),
                 xmlFreeDoc);
    if (!doc) throw std::runtime_error("Malformed XML feed");
    auto* root = xmlDocGetRootElement(doc.get());
    const auto root_name = name(root);
    bool atom = root_name == "feed";
    auto* parent = atom ? root : child(root, "channel");
    if ((!atom && root_name != "rss") || !parent) throw std::runtime_error("Response is not an RSS or Atom feed");
    std::vector<Story> stories;
    std::size_t count = 0;
    for (auto* entry = parent->children; entry && count < 300; entry = entry->next) {
        if (name(entry) != (atom ? "entry" : "item")) continue;
        ++count;
        Story story;
        story.source = feed.source;
        story.language = feed.language;
        story.seen = now;
        story.title = single_line(html_to_text(markup(child(entry, "title")))).substr(0, 1000);
        std::string link;
        if (atom) {
            for (auto* node = entry->children; node; node = node->next)
                if (name(node) == "link" && (attribute(node, "rel").empty() || attribute(node, "rel") == "alternate")) {
                    link = attribute(node, "href");
                    break;
                }
        } else
            link = single_line(content(child(entry, "link")));
        story.url = resolve_url(feed.url, link);
        std::vector<std::string> categories;
        for (auto* node = entry->children; node; node = node->next)
            if (name(node) == "category") categories.push_back(atom ? attribute(node, "term") : single_line(content(node)));
        if (story.title.empty() || story.url.empty() || !technology_story(feed, story.url, categories)) continue;
        auto date = content(child(entry, atom ? "published" : "pubDate"));
        if (date.empty() && atom) date = content(child(entry, "updated"));
        story.published = parse_date(date);
        if (story.published > now + 86400) story.published = now;
        story.summary = html_to_text(markup(child(entry, atom ? "summary" : "description"))).substr(0, 12000);
        auto full = html_to_text(markup(child(entry, atom ? "content" : "encoded")));
        if (!full.empty()) {
            if (story.summary.empty()) story.summary = full.substr(0, 12000);
            bool complete = full.size() >= 800 && std::count(full.begin(), full.end(), '\n') >= 4 && !incomplete(full);
            story.content = std::move(full);
            story.kind = complete ? ContentKind::Feed : ContentKind::Incomplete;
        }
        if (story.summary.empty()) story.summary = "No summary provided. Press Enter to load the public article.";
        stories.push_back(std::move(story));
    }
    return merge_stories(std::move(stories));
}

ArticleText extract_article(std::string_view html, std::string_view url) {
    auto doc = html_doc(html);
    if (!doc) return {};
    const auto host = url_host(url);
    std::string text;
    if (host == "arstechnica.com" || host == "www.arstechnica.com")
        text = select_text(doc.get(), "//*[contains(concat(' ', normalize-space(@class), ' '), ' article-content ') or contains(concat(' ', normalize-space(@class), ' '), ' post-content ')]");
    else if (host == "techcrunch.com" || host == "www.techcrunch.com")
        text = select_text(doc.get(), "//*[contains(concat(' ', normalize-space(@class), ' '), ' wp-block-post-content ') or contains(concat(' ', normalize-space(@class), ' '), ' entry-content ')]");
    else if (host == "tecnoblog.net")
        text = select_text(doc.get(), "//*[contains(concat(' ', normalize-space(@class), ' '), ' entry-content ') or contains(concat(' ', normalize-space(@class), ' '), ' post-content ')]");
    if (text.empty()) text = select_text(doc.get(), "//*[@itemprop='articleBody'] | //article | //*[@role='article']");
    if (text.size() < 120) return {};
    const bool partial = text.size() < 800 || incomplete(text) || std::count(text.begin(), text.end(), '\n') < 4;
    return {std::move(text), partial ? ContentKind::Incomplete : ContentKind::Extracted};
}
}  // namespace tech_news
