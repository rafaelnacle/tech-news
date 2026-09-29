# Contributor practices

Build with C++20 and CMake. Keep the executable name `tech-news` and the interface in English. The app needs no API keys, accounts, server, or `.env` file.

- Use RAII for curl handles, XML documents, SQLite statements, threads, files, and child processes. Prefer values and owning standard containers; do not leak ownership through raw pointers.
- Keep HTTP, parsing/extraction, persistence, application state, and terminal rendering in separate modules. Core behavior must be testable without a terminal or live publishers.
- Handle failures explicitly with contextual, sanitized messages. A failed source must not discard another source's results or usable cached stories. Do not swallow persistence errors.
- Bound requests with connect/total timeouts, download-size limits, limited redirects, HTTP(S)-only protocols, TLS verification, and cancellation. Never bypass access restrictions.
- Only the UI thread may own or mutate UI state. Workers publish immutable results through a synchronized queue and wake the UI using its supported event API. Cancel and join work before destroying the screen.
- Pin downloaded dependencies to a release and verified digest. Use system libcurl, libxml2, and SQLite with declared minimum versions. Do not add unpinned downloads or vendored credentials.
- Use synthetic offline fixtures for feed parsing, dates, Portuguese text, article extraction, filtering, security boundaries, and persistence. Use a local test server for networking failures and cancellation; tests must not depend on publisher availability.
- Preserve bookmarks and read state on refresh. Prune unbookmarked stories after 30 days; keep all application data outside the repository in platform user storage.
- Treat feeds, HTML, URLs, and cached text as untrusted. Disable XML external resources, sanitize terminal control sequences and malformed UTF-8, use parameterized SQL, and open browsers with argument-based process execution without a shell.
- Never log credentials, environment values, or sensitive URL parameters. Ignore local databases, cache, settings, secrets, and build products. Review tracked-file candidates for secrets before delivery.
- Keep README.md focused on description, prerequisites, installation, execution, configuration, controls, and troubleshooting. Contributor instructions belong here.
- Run the relevant build, fixture tests, and terminal smoke tests before committing. Use semantic local commit messages. The user performs all pushes; do not push or publish.
