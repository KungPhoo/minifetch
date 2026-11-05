#include "minifetch.h"
#include <emscripten/fetch.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
bool mainThread = true, failStart = false;
int closed = 0, calls = 0, checks = 0;
emscripten_fetch_attr_t pending{};
std::string pendingUrl;
void require(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
void complete(unsigned short status, std::string body) {
    auto* fetch = new emscripten_fetch_t{pending.userData, body.data(), body.size(), status};
    if (status >= 200 && status < 300) pending.onsuccess(fetch);
    else pending.onerror(fetch);
}
MiniFetch::Request request() {
    MiniFetch::Request rq;
    rq.fillServerFromUrl("https://example.test:8443/path?q=1");
    return rq;
}
}
extern "C" {
int emscripten_is_main_browser_thread() { return mainThread; }
void emscripten_fetch_attr_init(emscripten_fetch_attr_t* attr) { *attr = {}; }
emscripten_fetch_t* emscripten_fetch(emscripten_fetch_attr_t* attr, const char* url) {
    ++calls;
    if (failStart) return nullptr;
    pending = *attr; pendingUrl = url;
    if (attr->attributes & EMSCRIPTEN_FETCH_SYNCHRONOUS)
        return new emscripten_fetch_t{nullptr, "sync", 4, 201};
    // A non-null opaque handle; completion is explicitly scheduled by the test.
    return reinterpret_cast<emscripten_fetch_t*>(1);
}
int emscripten_fetch_close(emscripten_fetch_t* fetch) { ++closed; delete fetch; return 0; }
std::size_t emscripten_fetch_get_response_headers_length(emscripten_fetch_t*) { return sizeof("Location: /target\r\n") - 1; }
std::size_t emscripten_fetch_get_response_headers(emscripten_fetch_t*, char* dst, std::size_t size) {
    const char headers[] = "Location: /target\r\n";
    const auto copied = (std::min)(size - 1, sizeof(headers) - 1);
    std::memcpy(dst, headers, copied); dst[copied] = 0; return copied;
}
}
int main() {
    try {
        MiniFetch client;
        require(!client.fetch(request()).error.empty() && calls == 0, "sync main-thread fetch was started");
        bool called = false;
        {
            MiniFetch temporary;
            auto rq = request(); rq.rawPostData = std::string("a\0b", 3);
            rq.headers["X-Test"] = "custom";
            rq.timeoutSeconds = (std::numeric_limits<int>::max)();
            rq.withCredentials = false;
            temporary.fetchAsync(rq, [&](MiniFetch::Response response) {
                called = true;
                require(response.status == 418 && response.error.empty() && response.toString() == "error-body", "HTTP error body dropped");
                require(response.location == "/target", "browser location missing");
            });
        } // Source Request and client die before the callback.
        require(!called && calls == 1, "callback was not deferred");
        require(pendingUrl == "https://example.test:8443/path?q=1", "browser URL/port");
        require(std::string(pending.requestMethod) == "POST", "browser POST inference");
        require(pending.requestDataSize == 3 && std::string(pending.requestData, 3) == std::string("a\0b", 3), "body lifetime");
        require(pending.timeoutMSecs == (std::numeric_limits<uint32_t>::max)() && !pending.withCredentials, "timeout clamp/credentials");
        bool headerSeen = false;
        for (auto h = pending.requestHeaders; *h; h += 2) {
            require(std::string(*h) != "Host" && std::string(*h) != "Cookie" && std::string(*h) != "User-Agent", "browser forbidden header sent");
            if (std::string(*h) == "X-Test" && std::string(h[1]) == "custom") headerSeen = true;
        }
        require(headerSeen, "custom header lifetime");
        complete(418, "error-body");
        require(called && closed == 1, "callback/handle cleanup");
        auto rq = request(); rq.method = std::string(32, 'X');
        called = false;
        client.fetchAsync(rq, [&](MiniFetch::Response response) { called = true; require(!response.error.empty(), "oversized method accepted"); });
        require(called && calls == 1, "method buffer overflow guard");
        failStart = true; called = false;
        client.fetchAsync(request(), [&](MiniFetch::Response response) { called = true; require(!response.error.empty(), "start failure missing"); });
        require(called, "start failure callback missing");
        failStart = false; mainThread = false;
        auto response = client.fetch(request());
        require(response.status == 201 && response.toString() == "sync" && closed == 2, "worker synchronous response");
        mainThread = true; called = false;
        client.fetchAsync(request(), [&](MiniFetch::Response response) {
            called = true; require(response.status == MiniFetch::Status::InternalError && !response.error.empty(), "network failure missing");
        });
        complete(0, "");
        require(called && closed == 3, "network-error callback cleanup");
        std::cout << checks << " browser backend contract checks passed (test double)\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
