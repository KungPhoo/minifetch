#include "minifetch.h"
#include <emscripten/fetch.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
bool failStart = false;
int closed = 0, calls = 0, checks = 0, yields = 0;
unsigned short responseStatus = 418;
emscripten_fetch_attr_t pending{};
std::string pendingUrl;
void require(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
MiniFetch::Request request() {
    MiniFetch::Request rq;
    rq.fillServerFromUrl("https://example.test:8443/path?q=1");
    return rq;
}
}
extern "C" {
void emscripten_fetch_attr_init(emscripten_fetch_attr_t* attr) { *attr = {}; }
emscripten_fetch_t* emscripten_fetch(emscripten_fetch_attr_t* attr, const char* url) {
    ++calls;
    if (failStart) return nullptr;
    pending = *attr; pendingUrl = url;
    require(!(attr->attributes & EMSCRIPTEN_FETCH_SYNCHRONOUS), "browser fetch must be asynchronous");
    return reinterpret_cast<emscripten_fetch_t*>(1);
}
void emscripten_sleep(unsigned int) {
    ++yields;
    if (calls == 1) {
        MiniFetch reentrant;
        require(!reentrant.fetch(request()).error.empty() && calls == 1, "overlapping Asyncify fetch should fail before starting");
        require(std::string(pending.requestMethod) == "POST", "POST inference");
        require(pending.requestDataSize == 3 && std::string(pending.requestData, 3) == std::string("a\0b", 3), "body lifetime during wait");
        require(pending.timeoutMSecs == (std::numeric_limits<uint32_t>::max)() && !pending.withCredentials, "timeout/credentials");
        bool custom = false;
        for (auto h = pending.requestHeaders; *h; h += 2) {
            require(std::string(*h) != "Host" && std::string(*h) != "Cookie" && std::string(*h) != "User-Agent", "forbidden browser header");
            if (std::string(*h) == "X-Test" && std::string(h[1]) == "custom") custom = true;
        }
        require(custom, "header lifetime during wait");
    }
    // Completion happens only after fetch yields to the browser event loop.
    const char body[] = "error-body";
    auto* fetch = new emscripten_fetch_t{pending.userData, body, sizeof(body) - 1, responseStatus};
    if (responseStatus >= 200 && responseStatus < 300) pending.onsuccess(fetch);
    else pending.onerror(fetch);
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
        auto rq = request(); rq.rawPostData = std::string("a\0b", 3);
        rq.headers["X-Test"] = "custom";
        rq.timeoutSeconds = (std::numeric_limits<int>::max)();
        rq.withCredentials = false;
        auto response = client.fetch(rq);
        require(response.status == 418 && response.error.empty() && response.toString() == "error-body", "HTTP error body dropped");
        require(response.location == "/target", "browser location missing");
        require(calls == 1 && yields == 1 && closed == 1, "fetch did not wait for and clean up callback");
        require(pendingUrl == "https://example.test:8443/path?q=1", "browser URL/port");
        rq.method = std::string(32, 'X');
        require(!client.fetch(rq).error.empty() && calls == 1, "method buffer overflow guard");
        failStart = true;
        require(!client.fetch(request()).error.empty() && yields == 1, "start failure should not wait");
        failStart = false; responseStatus = 0;
        response = client.fetch(request());
        require(response.status == MiniFetch::Status::InternalError && !response.error.empty(), "network failure missing");
        responseStatus = 201;
        client.request = request();
        response = client.fetch();
        require(response.status == 201 && response.error.empty(), "legacy fetch API");
        require(closed == 3, "response handles leaked");
        std::cout << checks << " browser fetch contract checks passed (test double)\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
