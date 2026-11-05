// minifetch.cpp : lightweight web fetch library
#include "stdafx.h"
#include "minifetch.h"
#include <sstream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <ctime>
#include <locale>

#if defined(_WIN32)
    #include <windows.h>
    #include <winhttp.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "winhttp.lib")
    #endif
#elif defined(__EMSCRIPTEN__)
    #include <emscripten/fetch.h>
    #include <emscripten/threading.h>
#else
    #include <curl/curl.h>
#endif

static std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Set-Cookie: session=abc123; Path=/; HttpOnly; Secure
// produces
// response.setCookies["session"] = "abc123";
static bool parseSetCookie(
    const std::string& header,
    std::string& name,
    std::string& value) {
    const auto end = header.find(';');
    const auto pair = trim(header.substr(0, end));
    const auto equals = pair.find('=');

    if (equals == std::string::npos) {
        return false;
    }

    name = trim(pair.substr(0, equals));
    value = trim(pair.substr(equals + 1));

    return !name.empty();
}

namespace {
std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
bool validPort(const std::string& port) {
    if (port.empty()) return true;
    unsigned value = 0;
    if (port.size() > 5) return false;
    for (unsigned char c : port) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + c - '0';
    }
    return value > 0 && value <= 65535;
}
bool validHost(const std::string& host) {
    if (host.empty()) return false;
    for (unsigned char c : host)
        if (c <= 32 || c == 127 || c == '/' || c == '\\' || c == '?' || c == '#' || c == '@' || c == '%') return false;
    if (host.front() == '[')
        return host.back() == ']' && host.find(':') != std::string::npos &&
            host.find_first_not_of("0123456789abcdefABCDEF:.[]") == std::string::npos;
    return host.find_first_of(":[]") == std::string::npos;
}
bool token(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) && std::string("!#$%&'*+-.^_`|~").find(c) == std::string::npos) return false;
    }
    return true;
}
}

#ifdef __EMSCRIPTEN__
namespace {
struct BrowserRequest {
    std::string url, body;
    std::vector<std::string> headerStrings;
    std::vector<const char*> headers;
    std::function<void(MiniFetch::Response)> callback;
};

MiniFetch::Response browserResponse(emscripten_fetch_t* fetch) {
    MiniFetch::Response response;
    if (!fetch) { response.error = "Unable to start browser fetch"; return response; }
    if (fetch->status == 0) response.error = "Browser network, timeout or CORS failure";
    else response.status = static_cast<MiniFetch::Status::Code>(fetch->status);
    if (fetch->data && fetch->numBytes) response.bytes.assign(fetch->data, fetch->data + fetch->numBytes);
    const auto size = emscripten_fetch_get_response_headers_length(fetch);
    if (size) {
        std::vector<char> headers(size + 1, 0);
        emscripten_fetch_get_response_headers(fetch, headers.data(), headers.size());
        std::istringstream lines(headers.data());
        for (std::string line; std::getline(lines, line);) {
            const auto colon = line.find(':');
            if (colon != std::string::npos && lower(line.substr(0, colon)) == "location")
                response.location = trim(line.substr(colon + 1));
        }
    }
    // Browser security intentionally hides Set-Cookie and manages the cookie store itself.
    return response;
}

void browserDone(emscripten_fetch_t* fetch) {
    std::unique_ptr<BrowserRequest> state(static_cast<BrowserRequest*>(fetch->userData));
    MiniFetch::Response response;
    try { response = browserResponse(fetch); }
    catch (...) {
        emscripten_fetch_close(fetch);
        std::terminate(); // Never unwind through the C/JS callback boundary.
    }
    emscripten_fetch_close(fetch);
    try { state->callback(std::move(response)); }
    catch (...) { std::terminate(); } // Callbacks must not throw.
}

std::unique_ptr<BrowserRequest> browserSetup(const MiniFetch::Request& request,
        std::string url, std::string body, emscripten_fetch_attr_t& attr) {
    auto state = std::make_unique<BrowserRequest>();
    state->url = std::move(url); state->body = std::move(body);
    emscripten_fetch_attr_init(&attr);
    const auto method = lower(request.method);
    if (request.method.size() >= sizeof(attr.requestMethod) || method == "connect" || method == "trace" || method == "track") return nullptr;
    std::memcpy(attr.requestMethod, request.method.c_str(), request.method.size() + 1);
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE;
    attr.withCredentials = request.withCredentials;
    attr.timeoutMSecs = static_cast<uint32_t>(request.timeoutSeconds) > (std::numeric_limits<uint32_t>::max)() / 1000
        ? (std::numeric_limits<uint32_t>::max)() : static_cast<uint32_t>(request.timeoutSeconds) * 1000;
    for (const auto& header : request.headers) {
        const auto name = lower(header.first);
        // XHR owns these headers; trying to set them is forbidden by the browser.
        if (name == "host" || name == "user-agent" || name == "cookie") continue;
        state->headerStrings.push_back(header.first);
        state->headerStrings.push_back(header.second);
    }
    for (const auto& header : state->headerStrings) state->headers.push_back(header.c_str());
    state->headers.push_back(nullptr);
    attr.requestHeaders = state->headers.data();
    attr.requestData = state->body.empty() ? nullptr : state->body.data();
    attr.requestDataSize = state->body.size();
    return state;
}
}

void MiniFetch::fetchAsync(Request input, std::function<void(Response)> callback) {
    if (!callback) return;
    MiniFetch operation(cookies_);
    operation.request = std::move(input);
    Response response;
    std::string body;
    if (!operation.prepare(response, body)) { callback(std::move(response)); return; }
    emscripten_fetch_attr_t attr;
    auto state = browserSetup(operation.request, operation.buildUrl(), std::move(body), attr);
    if (!state) { response.error = "HTTP method is too long or forbidden by the browser"; callback(std::move(response)); return; }
    state->callback = std::move(callback);
    attr.userData = state.get();
    attr.onsuccess = browserDone;
    attr.onerror = browserDone;
    // The async state owns every pointer supplied to Fetch until completion.
    auto* raw = state.release();
    if (!emscripten_fetch(&attr, raw->url.c_str())) {
        std::unique_ptr<BrowserRequest> failed(raw);
        response.error = "Unable to start browser fetch";
        failed->callback(std::move(response));
    }
}
#endif

namespace {
bool cookieValue(const std::string& value) {
    for (unsigned char c : value)
        if (c < 0x21 || c > 0x7e || c == '"' || c == ',' || c == ';' || c == '\\') return false;
    return true;
}
std::string requestPath(const MiniFetch::Request& rq) {
    std::string path = rq.path.substr(0, rq.path.find_first_of("?#"));
    if (path.empty() || path.front() != '/') path.insert(0, "/");
    return path;
}
std::optional<std::chrono::system_clock::time_point> cookieDate(const std::string& text) {
    // RFC 1123 dates are UTC, independent of the host's timezone.
    std::tm tm = {};
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail()) return std::nullopt;
    int year = tm.tm_year + 1900;
    const unsigned month = tm.tm_mon + 1;
    if (year < 1601 || month > 12 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59) return std::nullopt;
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + tm.tm_mday - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const auto days = static_cast<long long>(era) * 146097 + doe - 719468;
    const auto seconds = days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    // Clamp extreme dates before converting to the clock's potentially narrower duration.
    const auto maxSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::duration::max()).count();
    const auto minSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::duration::min()).count();
    if (seconds >= maxSeconds) return std::chrono::system_clock::time_point::max();
    if (seconds <= minSeconds) return std::chrono::system_clock::time_point::min();
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}
}

bool MiniFetch::Request::fillServerFromUrl(std::string url) {
    clear();
    if (url.find('\0') != std::string::npos || url.find_first_of("\r\n") != std::string::npos) return false;
    Request parsed;
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    parsed.protocol = lower(url.substr(0, schemeEnd));
    if (parsed.protocol != "http" && parsed.protocol != "https") return false;
    const auto start = schemeEnd + 3;
    const auto end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) return false;
        parsed.server = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':' || close + 2 == authority.size()) return false;
            parsed.port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.find(':');
        parsed.server = authority.substr(0, colon);
        if (colon != std::string::npos) {
            parsed.port = authority.substr(colon + 1);
            if (parsed.port.empty()) return false;
        }
    }
    if (!validHost(parsed.server) || !validPort(parsed.port)) return false;
    if (parsed.server.find_first_of(" \t\r\n\\") != std::string::npos) return false;
    const auto fragment = url.find('#', start);
    if (end != std::string::npos && end != fragment) {
        parsed.path = url.substr(end, fragment == std::string::npos ? fragment : fragment - end);
        if (parsed.path.front() == '?') parsed.path.insert(0, "/");
    }
    *this = std::move(parsed);
    return true;
}

MiniFetch::Response MiniFetch::fetch() { return fetch(request); }

MiniFetch::Response MiniFetch::fetch(Request input) {
    MiniFetch operation(cookies_);
    operation.request = std::move(input);
    return operation.fetchImpl();
}

bool MiniFetch::prepare(Response& response, std::string& body) {
    request.protocol = lower(request.protocol);
    if ((request.protocol != "http" && request.protocol != "https") ||
        !validHost(request.server) ||
        !validPort(request.port) || request.timeoutSeconds < 0 || !token(request.method)) {
        response.error = "Invalid request URL, method, port or timeout";
        return false;
    }
    if ((!request.postVariables.empty() || !request.postFileNames.empty() || !request.rawPostData.empty()) && request.method == "GET")
        request.method = "POST";
    if (!request.postFileNames.empty() &&
        (request.boundary.empty() || request.boundary.size() > 70 ||
         request.boundary.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)) {
        response.error = "Invalid multipart boundary";
        return false;
    }
    try { body = buildQueryPost() + request.rawPostData; }
    catch (const std::runtime_error& error) { response.error = error.what(); return false; }
    for (const auto& header : request.headers) {
        if (!token(header.first) || header.second.find_first_of("\r\n") != std::string::npos ||
            header.second.find('\0') != std::string::npos) {
            response.error = "Invalid HTTP header";
            return false;
        }
        const auto name = lower(header.first);
        if (name == "content-length" || name == "transfer-encoding") {
            response.error = "Body framing headers are managed by the transport";
            return false;
        }
    }
    if (request.path.find_first_of("\r\n") != std::string::npos || request.path.find('\0') != std::string::npos) {
        response.error = "Invalid request path";
        return false;
    }
    return true;
}

MiniFetch::Response MiniFetch::fetchImpl() {
    MiniFetch::Response response;
    std::string postData;
    if (!prepare(response, postData)) return response;

#if defined(_WIN32)

    struct HandleGuard {
        HINTERNET value;
        ~HandleGuard() { if (value) WinHttpCloseHandle(value); }
    };
    // ==
    auto toWide = [](const std::string& input) -> std::wstring {
        if (input.empty()) {
            return std::wstring();
        }
        int wide_len = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
        if (wide_len == 0) {
            return std::wstring();
        }
        std::wstring result(wide_len, 0);
        MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, &result[0], wide_len);
        // Remove the null terminator that was included by MultiByteToWideChar
        result.pop_back();
        return result;
    };  // ==

    std::string host = request.server;
    if (host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    std::wstring wServer = toWide(host);
    INTERNET_PORT port = request.protocol == "https" ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (!request.port.empty()) {
        port = std::stoi(request.port);
    }

    std::string url = buildUrl(false);
    std::wstring wPath = toWide(url);

    HINTERNET hSession = WinHttpOpen(L"WinHTTP Client/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HandleGuard sessionGuard{hSession};
    if (!hSession) { response.error = "WinHttpOpen failed"; return response; }

    const int timeoutMs = request.timeoutSeconds > (std::numeric_limits<int>::max)() / 1000
        ? (std::numeric_limits<int>::max)() : request.timeoutSeconds * 1000;
    if (!WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs)) {
        response.error = "WinHttpSetTimeouts failed"; return response;
    }
    HINTERNET hConnect = WinHttpConnect(hSession, wServer.c_str(), port, 0);
    HandleGuard connectGuard{hConnect};
    if (!hConnect) {

        response.error = "WinHTTP connection/request failed";
        return response;
    }

    DWORD flags = (request.protocol == "https") ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                            toWide(request.method).c_str(),
                                            wPath.c_str(),
                                            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    HandleGuard requestGuard{hRequest};
    if (!hRequest) {

        response.error = "WinHTTP connection/request failed";
        return response;
    }

    // do not automatically resolve 302 redirects
    DWORD disable = WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) {
        response.error = "Unable to disable WinHTTP's separate cookie store"; return response;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) {
        response.error = "Unable to disable automatic redirects"; return response;
    }

    // Set headers
    for (auto& [k, v] : request.headers) {
        std::string headerLine(k + ": " + v + "\r\n");
        std::wstring wheaderLine = toWide(headerLine);
        if (!WinHttpAddRequestHeaders(hRequest, wheaderLine.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD)) {
            response.error = "WinHttpAddRequestHeaders failed"; return response;
        }
    }

    if (postData.size() > (std::numeric_limits<DWORD>::max)()) {

        response.error = "Request body exceeds WinHTTP size limit";
        return response;
    }
    BOOL bResults = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        postData.empty() ? WINHTTP_NO_REQUEST_DATA : static_cast<LPVOID>(postData.data()),
        static_cast<DWORD>(postData.size()), static_cast<DWORD>(postData.size()), 0);
    if (!bResults) {

        response.error = "WinHttpSendRequest failed";
        return response;
    }

    bResults = WinHttpReceiveResponse(hRequest, NULL);
    if (!bResults) {

        response.error = "WinHTTP connection/request failed";
        return response;
    }

    // Receive Headers/Cookies
    DWORD index = 0;

    auto queryHeader = [&index, hRequest](DWORD query, std::string& value) -> bool {
        DWORD bytes = 0;
        value.clear();

        // First call obtains the required buffer size.
        WinHttpQueryHeaders(
            hRequest,
            query,
            WINHTTP_HEADER_NAME_BY_INDEX,
            nullptr,
            &bytes,
            &index);

        DWORD err = GetLastError();

        if (err == ERROR_WINHTTP_HEADER_NOT_FOUND) {
            return false;  // No more Set-Cookie headers
        }

        if (err != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }

        std::vector<wchar_t> wbuffer(bytes / sizeof(wchar_t) + 1, L'\0');

        if (!WinHttpQueryHeaders(
                hRequest,
                query,
                WINHTTP_HEADER_NAME_BY_INDEX,
                wbuffer.data(),
                &bytes,
                &index)) {
            return false;
        }

        std::wstring wideCookie(wbuffer.data());

        // Convert the header value to UTF-8.
        int len = WideCharToMultiByte(
            CP_UTF8, 0, wideCookie.c_str(), -1,
            nullptr, 0, nullptr, nullptr);

        if (len <= 1) {
            return false;  // header is present, but has no value
        }

        std::string utf8(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, wideCookie.c_str(), -1,
            utf8.data(), len, nullptr, nullptr);
        utf8.pop_back();  // Remove the terminating null.
        value = utf8;
        return true;
    };

    index = 0;
    for (;;) {
        std::string cookie;
        if (!queryHeader(WINHTTP_QUERY_SET_COOKIE, cookie)) {
            break;
        }

        std::string name, value;

        if (parseSetCookie(cookie, name, value)) {
            response.setCookies[name] = value;
            response.setCookieHeaders.push_back(cookie);
        }
    }

    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    do {
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            response.error = "WinHttpQueryDataAvailable failed";
            break;
        }
        if (dwSize == 0) {
            break;
        }

        std::vector<uint8_t> buffer(dwSize);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
            response.error = "WinHttpReadData failed";
            break;
        }

        if (dwDownloaded == 0) break;
        response.bytes.insert(response.bytes.end(), buffer.begin(), buffer.begin() + dwDownloaded);
    } while (dwSize > 0);

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        response.error = "WinHttpQueryHeaders failed";
    } else response.status = Status::Code(statusCode);

    // query location for redirects
    if (response.status >= 300 && response.status < 400) {
        index = 0;
        queryHeader(WINHTTP_QUERY_LOCATION, response.location);
    }

#elif defined(__EMSCRIPTEN__)
    if (emscripten_is_main_browser_thread()) {
        response.error = "Synchronous Fetch is unavailable on the browser main thread; use fetchAsync";
        return response;
    }
    emscripten_fetch_attr_t attr;
    auto state = browserSetup(request, buildUrl(), std::move(postData), attr);
    if (!state) { response.error = "HTTP method is too long or forbidden by the browser"; return response; }
    attr.attributes |= EMSCRIPTEN_FETCH_SYNCHRONOUS;
    std::unique_ptr<emscripten_fetch_t, decltype(&emscripten_fetch_close)>
        fetch(emscripten_fetch(&attr, state->url.c_str()), emscripten_fetch_close);
    response = browserResponse(fetch.get());

#else
    // C++ local-static initialization is once-only and synchronized.
    struct CurlRuntime {
        CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        ~CurlRuntime() { if (result == CURLE_OK) curl_global_cleanup(); }
    };
    static CurlRuntime runtime;
    if (runtime.result != CURLE_OK) { response.error = "curl_global_init failed"; return response; }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) { response.error = "curl_easy_init failed"; return response; }
    std::string url = buildUrl();
    char errorBuffer[CURL_ERROR_SIZE] = {};
    auto set = [&](CURLoption option, auto value) {
        const auto result = curl_easy_setopt(curl.get(), option, value);
        if (result != CURLE_OK) throw std::runtime_error(curl_easy_strerror(result));
    };
    struct HeaderList {
        curl_slist* value = nullptr;
        ~HeaderList() { curl_slist_free_all(value); }
    } headers;
    try {
        set(CURLOPT_ERRORBUFFER, errorBuffer);
        set(CURLOPT_URL, url.c_str());
        set(CURLOPT_CUSTOMREQUEST, request.method.c_str());
        set(CURLOPT_TIMEOUT, static_cast<long>(request.timeoutSeconds));
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_FOLLOWLOCATION, 0L);
        if (!postData.empty() || request.method == "POST" || request.method == "PUT" || request.method == "PATCH") {
            set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(postData.size()));
            set(CURLOPT_POSTFIELDS, postData.data());
        }
        if (request.method == "HEAD") set(CURLOPT_NOBODY, 1L);
        for (const auto& header : request.headers) {
            const auto line = header.first + ": " + header.second;
            auto* added = curl_slist_append(headers.value, line.c_str());
            if (!added) throw std::runtime_error("Unable to allocate HTTP headers");
            headers.value = added;
        }
        if (headers.value) set(CURLOPT_HTTPHEADER, headers.value);
        auto write = +[](char* ptr, size_t size, size_t count, void* user) noexcept -> size_t {
            const auto length = size * count;
            try {
                auto& bytes = static_cast<Response*>(user)->bytes;
                bytes.insert(bytes.end(), ptr, ptr + length);
                return length;
            } catch (...) { return 0; } // Never unwind through a C callback.
        };
        auto header = +[](char* ptr, size_t size, size_t count, void* user) noexcept -> size_t {
            const auto length = size * count;
            try {
                auto& response = *static_cast<Response*>(user);
                const std::string line(ptr, length);
                // A proxy CONNECT or interim response must not leak headers into the final response.
                if (line.compare(0, 5, "HTTP/") == 0) {
                    response.setCookies.clear(); response.setCookieHeaders.clear(); response.location.clear();
                }
                const auto colon = line.find(':');
                if (colon != std::string::npos) {
                    const auto name = lower(line.substr(0, colon));
                    if (name == "set-cookie") {
                        std::string key, value;
                        if (parseSetCookie(line.substr(colon + 1), key, value)) {
                            response.setCookies[key] = value;
                            response.setCookieHeaders.push_back(trim(line.substr(colon + 1)));
                        }
                    } else if (name == "location") response.location = trim(line.substr(colon + 1));
                }
                return length;
            } catch (...) { return 0; }
        };
        set(CURLOPT_WRITEFUNCTION, write);
        set(CURLOPT_WRITEDATA, &response);
        set(CURLOPT_HEADERFUNCTION, header);
        set(CURLOPT_HEADERDATA, &response);
        const auto result = curl_easy_perform(curl.get());
        if (result == CURLE_OK) {
            long status = 0;
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
            response.status = static_cast<Status::Code>(status);
        } else response.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(result);
    } catch (const std::runtime_error& error) { response.error = error.what(); }
#endif

    // finally
    storeReceivedCookies(request, response);
    return response;
}

std::string MiniFetch::urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (const unsigned char c : value) {
        if (
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(c);
        }
    }
    return escaped.str();
}

// might add more headers
std::string MiniFetch::buildQueryPost() {
    auto fill = [&](std::string name, std::string def) {
        if (std::none_of(request.headers.begin(), request.headers.end(), [&](const auto& h) { return lower(h.first) == lower(name); })) {
            request.headers[name] = def;
        }
    };
    fill("User-Agent", "MiniFetch");
    fill("Host", request.server + (request.port.empty() ? "" : ":" + request.port));
    fill("Accept", "*/*");
    if (!request.rawPostData.empty() && request.postVariables.empty() && request.postFileNames.empty())
        fill("Content-Type", "application/octet-stream");

    std::string cookieHeader = buildCookieHeader();
    if (!cookieHeader.empty()) {
        for (auto it = request.headers.begin(); it != request.headers.end();) {
            if (lower(it->first) == "cookie") it = request.headers.erase(it);
            else ++it;
        }
        request.headers["Cookie"] = cookieHeader;
    }

    std::string query;
    if (request.postFileNames.empty()) {
        if (request.postVariables.empty()) {
        } else {
            fill("Content-Type", "application/x-www-form-urlencoded");
            for (auto it = request.postVariables.begin(); it != request.postVariables.end(); ++it) {
                if (it != request.postVariables.begin()) {
                    query += "&";
                }
                query += urlEncode(it->first) + "=" + urlEncode(it->second);
            }
        }
    } else {
        // multipart/form-data
        request.headers["Content-Type"] = "multipart/form-data; boundary=" + request.boundary;

        for (auto it = request.postVariables.begin(); it != request.postVariables.end(); ++it) {
            query += "--" + request.boundary + "\r\nContent-Disposition: form-data; name=\"" + urlEncode(it->first) + "\"\r\n\r\n" + it->second + "\r\n";
        }
        for (auto it = request.postFileNames.begin(); it != request.postFileNames.end(); ++it) {
            std::string filename = it->second;
            size_t fnat = 0;
            for (size_t i = 0; i < filename.size(); ++i) {
                if (filename[i] == '/' || filename[i] == '\\') {
                    fnat = i + 1;
                }
            }
            filename = filename.substr(fnat);
            std::string contentType = "application/octet-stream";
            query += "--" + request.boundary + "\r\n" + "Content-Disposition: form-data; name=\"" + urlEncode(it->first) + "\"; filename=\"" + urlEncode(filename) + "\"\r\n" + "Content-Type: " + contentType + "\r\n" + "\r\n" + fileContents(it->second) + "\r\n";
            ;
        }

        query += "--" + request.boundary + "--\r\n";
    }
    return query;
}

std::string MiniFetch::buildQueryGet() {
    std::string query;
    for (auto it = request.getVariables.begin(); it != request.getVariables.end(); ++it) {
        if (it != request.getVariables.begin()) {
            query += "&";
        }
        query += urlEncode(it->first) + "=" + urlEncode(it->second);
    }
    return query;
}

std::map<std::string, std::string> MiniFetch::getRequestCookies() const {
    std::map<std::string, std::string> cookies;

    // load cookie jar entries
    {
        std::lock_guard<std::mutex> lock(cookies_->mutex);
        auto find = cookies_->values.find(cookieOrigin(request));
        if (find != cookies_->values.end()) {
            const auto now = std::chrono::system_clock::now();
            const auto path = requestPath(request);
            // Longer paths override less specific cookies with the same name.
            for (const auto& entry : find->second) {
                const auto& cookiePath = entry.first.first;
                const auto& cookie = entry.second;
                const bool matches = path.compare(0, cookiePath.size(), cookiePath) == 0 &&
                    (path.size() == cookiePath.size() || cookiePath.back() == '/' || path[cookiePath.size()] == '/');
                if (matches && (!cookie.secure || request.protocol == "https") &&
                    (!cookie.expires || *cookie.expires > now)) cookies[entry.first.second] = cookie.value;
            }
        }
    }

    // add/overwrite new presented cookies
    for (auto pair : request.cookies) {
        cookies[pair.first] = pair.second;
    }
    return cookies;
}

std::string MiniFetch::buildCookieHeader() const {
    auto cookies = getRequestCookies();

    std::string result;

    for (const auto& [name, value] : cookies) {
        if (!result.empty()) {
            result += "; ";
        }

        if (!token(name) || !cookieValue(value)) throw std::runtime_error("Invalid cookie name or value");
        result += name + "=" + value;
    }

    return result;
}

std::string MiniFetch::fileContents(std::string path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open upload file: " + path);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) throw std::runtime_error("Cannot read upload file: " + path);
    return ss.str();
}

std::string MiniFetch::buildUrl(bool withServer) {
    std::string path = request.path.substr(0, request.path.find('#'));
    if (path.empty() || path.front() != '/') path.insert(0, "/");
    const auto query = buildQueryGet();
    if (!query.empty()) path += (path.find('?') == std::string::npos ? "?" : "&") + query;
    if (!withServer) return path;
    return request.protocol + "://" + request.server +
        (request.port.empty() ? "" : ":" + request.port) + path;
}

// store the received cookies in our jar
void MiniFetch::storeReceivedCookies(const MiniFetch::Request& rq, const MiniFetch::Response& rs) {
    if (!rs.error.empty() || rs.setCookieHeaders.empty()) return;
    std::lock_guard<std::mutex> lock(cookies_->mutex);
    auto& jar = cookies_->values[cookieOrigin(rq)];
    const auto now = std::chrono::system_clock::now();
    for (auto it = jar.begin(); it != jar.end();) {
        if (it->second.expires && *it->second.expires <= now) it = jar.erase(it);
        else ++it;
    }
    for (const auto& header : rs.setCookieHeaders) {
        std::string name, value;
        if (!parseSetCookie(header, name, value) || !token(name) || !cookieValue(value)) continue;
        CookieState::Cookie cookie;
        cookie.value = value;
        auto path = requestPath(rq);
        path = path.substr(0, path.rfind('/'));
        if (path.empty()) path = "/";
        std::optional<long long> maxAge;
        std::istringstream attributes(header.substr(header.find(';') == std::string::npos ? header.size() : header.find(';') + 1));
        for (std::string attribute; std::getline(attributes, attribute, ';');) {
            const auto equals = attribute.find('=');
            const auto key = lower(trim(attribute.substr(0, equals)));
            const auto val = equals == std::string::npos ? "" : trim(attribute.substr(equals + 1));
            if (key == "secure") cookie.secure = true;
            else if (key == "path" && !val.empty() && val.front() == '/') path = val;
            else if (key == "expires") cookie.expires = cookieDate(val);
            else if (key == "max-age") {
                try {
                    size_t used = 0;
                    const auto age = std::stoll(val, &used);
                    if (used == val.size()) maxAge = age;
                } catch (const std::exception&) { /* Ignore invalid attributes. */ }
            }
        }
        if (maxAge) {
            if (*maxAge <= 0) cookie.expires = now;
            else {
                const auto room = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::time_point::max() - now).count();
                cookie.expires = now + std::chrono::seconds((std::min)(*maxAge, static_cast<long long>(room)));
            }
        }
        // Reject Secure cookies received over an insecure channel.
        if (cookie.secure && rq.protocol != "https") continue;
        const auto key = std::make_pair(path, name);
        if (cookie.expires && *cookie.expires <= now) jar.erase(key);
        else jar[key] = std::move(cookie);
    }
}

std::string MiniFetch::cookieOrigin(const Request& rq) {
    return lower(rq.protocol) + "://" + lower(rq.server) + ":" +
        (rq.port.empty() ? (rq.protocol == "https" ? "443" : "80") : rq.port);
}

std::map<std::string, std::map<std::string, std::string>> MiniFetch::cookieJar() const {
    std::lock_guard<std::mutex> lock(cookies_->mutex);
    std::map<std::string, std::map<std::string, std::string>> result;
    const auto now = std::chrono::system_clock::now();
    for (const auto& origin : cookies_->values)
        for (const auto& entry : origin.second)
            if (!entry.second.expires || *entry.second.expires > now)
                result[origin.first][entry.first.second] = entry.second.value;
    return result;
}
void MiniFetch::clearCookies() {
    std::lock_guard<std::mutex> lock(cookies_->mutex);
    cookies_->values.clear();
}

std::string MiniFetch::Response::statusString() const {
#define MCASE(a)    \
    case Status::a: \
        return std::string(#a);
    switch (status) {
        MCASE(InternalError)                //  -1
        MCASE(Continue)                     //  100
        MCASE(Processing)                   //  102
        MCASE(SwitchingProtocols)           //  101
        MCASE(Accepted)                     //  202
        MCASE(AlreadyReported)              //  208
        MCASE(Created)                      //  201
        MCASE(IMUsed)                       //  229
        MCASE(MultiStatus)                  //  207
        MCASE(NoContent)                    //  204
        MCASE(NonAuthoritativeInfo)         //  203
        MCASE(OK)                           //  200
        MCASE(PartialContent)               //  206
        MCASE(ResetContent)                 //  205
        MCASE(Found)                        //  302
        MCASE(MovedPermanently)             //  301
        MCASE(MultipleChoices)              //  300
        MCASE(NotModified)                  //  304
        MCASE(Permanent)                    //  308
        MCASE(SeeOther)                     //  303
        MCASE(SwitchProxy)                  //  306
        MCASE(Temp)                         //  307
        MCASE(UseProxy)                     //  305
        MCASE(BadRequest)                   //  400
        MCASE(Conflict)                     //  409
        MCASE(ExpectationFailed)            //  417
        MCASE(FailedDependency)             //  424
        MCASE(Forbidden)                    //  403
        MCASE(Gone)                         //  410
        MCASE(ImATeapot)                    //  418
        MCASE(LengthRequired)               //  411
        MCASE(Locked)                       //  423
        MCASE(LoginTimeOut)                 //  440
        MCASE(MethodNotAllowed)             //  405
        MCASE(MisdirectedRequest)           //  421
        MCASE(NotAcceptable)                //  406
        MCASE(NotFound)                     //  404
        MCASE(PayloadTooLarge)              //  413
        MCASE(PaymentRequired)              //  402
        MCASE(PreconditionFailed)           //  412
        MCASE(PreconditionRequired)         //  428
        MCASE(ProxyAuthRequired)            //  407
        MCASE(RangeNotSatisfiable)          //  416
        MCASE(RequestHeaderFieldsTooLarge)  //  431
        MCASE(RequestTimeout)               //  408
        MCASE(RetryWith)                    //  449
        MCASE(TooManyRequests)              //  429
        MCASE(Unauthorized)                 //  401
        MCASE(UnavailableForLegalReasons)   //  451
        MCASE(UnprocessableEntity)          //  422
        MCASE(UnsupportedMediaType)         //  415
        MCASE(UpgradeRequired)              //  426
        MCASE(URITooLong)                   //  414
        MCASE(BadGateway)                   //  502
        MCASE(BandwidthLimitExceeded)       //  509
        MCASE(GatewayTimeout)               //  504
        MCASE(HTTPVersionNotSupported)      //  505
        MCASE(InsufficientStorage)          //  507
        MCASE(Internal)                     //  500
        MCASE(LoopDetected)                 //  508
        MCASE(NetworkAuthRequired)          //  511
        MCASE(NotExtended)                  //  510
        MCASE(NotImplemented)               //  501
        MCASE(ServiceUnavailable)           //  503
        MCASE(VariantAlsoNegotiates)        //  506
#undef MCASE
    };
    std::stringstream ss;
    ss << "Status" << int(status);
    return ss.str();
}
