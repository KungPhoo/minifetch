// minifetch.cpp : lightweight web fetch library
#include "stdafx.h"
#include "minifetch.h"
#include <sstream>
#include <fstream>
#include <iomanip>

#if defined(_WIN32)
    #include <windows.h>
    #include <winhttp.h>
    #pragma comment(lib, "winhttp.lib")
#elif defined(__EMSCRIPTEN__)
    #include <emscripten/fetch.h>
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

bool MiniFetch::Request::fillServerFromUrl(std::string url) {
    clear();
    size_t protEnd = url.find("://");
    if (protEnd == std::string::npos) {
        return false;
    }
    protocol = url.substr(0, protEnd);

    size_t serverStart = protEnd + 3;

    size_t pathStart = url.find('/', serverStart);
    if (pathStart == std::string::npos) {
        // No path specified, entire rest is server
        server = url.substr(serverStart);
        path = "/";
    } else {
        server = url.substr(serverStart, pathStart - serverStart);
        path = url.substr(pathStart);
    }
    method = "GET";
    return true;
}

MiniFetch::Response MiniFetch::fetch() {
    MiniFetch::Response response = {};

    if ((!request.postVariables.empty() || !request.postFileNames.empty()) && request.method == "GET") {
        request.method = "POST";
    }

    // printf("Method: %s\n", request.method.c_str());

#if defined(_WIN32)

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

    std::wstring wServer = toWide(request.server);
    INTERNET_PORT port = request.protocol == "https" ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (!request.port.empty()) {
        port = std::stoi(request.port);
    }

    std::string url = buildUrl(false);
    std::wstring wPath = toWide(url);

    HINTERNET hSession = WinHttpOpen(L"WinHTTP Client/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        return {};
    }

    HINTERNET hConnect = WinHttpConnect(hSession, wServer.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD flags = (request.protocol == "https") ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                            toWide(request.method).c_str(),
                                            wPath.c_str(),
                                            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // do not automatically resolve 302 redirects
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(
        hRequest,
        WINHTTP_OPTION_REDIRECT_POLICY,
        &policy,
        sizeof(policy));

    // Set headers
    std::string postData = buildQueryPost();  // build post data before headers -> will add headers
    for (auto& [k, v] : request.headers) {
        std::string headerLine(k + ": " + v + "\r\n");
        std::wstring wheaderLine = toWide(headerLine);
        WinHttpAddRequestHeaders(hRequest, wheaderLine.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
        // printf("    Header: %s: %s\n", k.c_str(), v.c_str());
    }

    postData += request.rawPostData;
    // printf("    Sent-POST-Data:\n%s\n\n", postData.c_str());
    BOOL bResults = FALSE;
    if (!postData.empty()) {
        bResults = WinHttpSendRequest(hRequest,
                                      WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      request.method == "POST" ? (LPVOID)postData.c_str() : WINHTTP_NO_REQUEST_DATA,
                                      request.method == "POST" ? (DWORD)postData.size() : 0,
                                      request.method == "POST" ? (DWORD)postData.size() : 0,
                                      0);

        if (!bResults) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return {};
        }
    }

    bResults = WinHttpReceiveResponse(hRequest, NULL);
    if (!bResults) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Receive Headers/Cookies
    DWORD index = 0;

    auto queryHeader = [&index, hRequest](const char* name, std::string& value) -> bool {
        DWORD bytes = 0;
        value.clear();

        // First call obtains the required buffer size.
        WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_SET_COOKIE,
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
                WINHTTP_QUERY_SET_COOKIE,
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
        // DWORD bytes = 0;
        //
        // // First call obtains the required buffer size.
        // WinHttpQueryHeaders(
        //     hRequest,
        //     WINHTTP_QUERY_SET_COOKIE,
        //     WINHTTP_HEADER_NAME_BY_INDEX,
        //     nullptr,
        //     &bytes,
        //     &index);
        //
        // DWORD err = GetLastError();
        //
        // if (err == ERROR_WINHTTP_HEADER_NOT_FOUND) {
        //     break; // No more Set-Cookie headers
        // }
        //
        // if (err != ERROR_INSUFFICIENT_BUFFER) {
        //     break;
        // }
        //
        // std::vector<wchar_t> wbuffer(
        //     bytes / sizeof(wchar_t) + 1, L'\0');
        //
        // if (!WinHttpQueryHeaders(
        //         hRequest,
        //         WINHTTP_QUERY_SET_COOKIE,
        //         WINHTTP_HEADER_NAME_BY_INDEX,
        //         wbuffer.data(),
        //         &bytes,
        //         &index)) {
        //     break;
        // }
        //
        // std::wstring wideCookie(wbuffer.data());
        //
        // // Convert the header value to UTF-8.
        // int len = WideCharToMultiByte(
        //     CP_UTF8, 0, wideCookie.c_str(), -1,
        //     nullptr, 0, nullptr, nullptr);
        //
        // if (len <= 1) {
        //     continue;
        // }
        //
        // std::string cookie(static_cast<size_t>(len), '\0');
        // WideCharToMultiByte(
        //     CP_UTF8, 0, wideCookie.c_str(), -1,
        //     cookie.data(), len, nullptr, nullptr);
        // cookie.pop_back(); // Remove the terminating null.

        std::string cookie;
        if (!queryHeader("SetCookie", cookie)) {
            break;
        }

        // printf("    Response-Header:%s\n", cookie.c_str());

        std::string name, value;

        if (parseSetCookie(cookie, name, value)) {
            response.setCookies[name] = value;
        } else {
            parseSetCookie(cookie, name, value);
        }
    }

    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    do {
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            break;
        }
        if (dwSize == 0) {
            break;
        }

        std::vector<uint8_t> buffer(dwSize);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
            break;
        }

        response.bytes.insert(response.bytes.end(), buffer.begin(), buffer.begin() + dwDownloaded);
    } while (dwSize > 0);

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.status = Status::Code(statusCode);

    // query location for redirects
    if (response.status == 302) {
        index = 0;
        queryHeader("Location", response.location);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // printf("Response-Code: %d\n", int(statusCode));

    // printf("Response-Data:\n%s\n\n", response.toString().c_str());

#elif defined(__EMSCRIPTEN__)
    // In order to use the Fetch API, you would need to compile your code with -sFETCH
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, request.method.c_str());

    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;  // Synchronous!
    attr.withCredentials = true;                                                       // credentialed cross-origin requests - Cookies

    std::string fullUrl = buildUrl();
    emscripten_fetch_t* fetch = emscripten_fetch(&attr, fullUrl);
    // TODO postData
    if (fetch->status == 200) {
        response.bytes.assign(fetch->data, fetch->data + fetch->numBytes);
    }

    int status = fetch->status;
    emscripten_fetch_close(fetch);
    response.status = Status::Code(status);

#else
    CURL* curl = curl_easy_init();
    if (!curl) {
        return {};
    }

    std::string url = buildUrl();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeoutSeconds);

    std::string postData = buildQueryPost();  // post data before headers -> will add headers
    struct curl_slist* headers = nullptr;
    for (auto& [k, v] : request.headers) {
        std::string line = k + ": " + v;
        headers = curl_slist_append(headers, line.c_str());
    }
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* out = static_cast<std::vector<uint8_t>*>(userdata);
            size_t total = size * nmemb;
            out->insert(out->end(), (uint8_t*)ptr, (uint8_t*)ptr + total);
            return total; });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.bytes);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        const size_t len = size * nmemb;
        auto* response = static_cast<Response*>(userdata);

        std::string line(ptr, len);
        const auto colon = line.find(':');

        if (colon != std::string::npos) {
            std::string headerName = line.substr(0, colon);

            std::transform(headerName.begin(), headerName.end(),
                           headerName.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });

            if (headerName == "set-cookie") {
                std::string name, value;

                if (parseSetCookie(line.substr(colon + 1),
                                   name, value)) {
                    response->setCookies[name] = value;
                }
            }
        }

        return len; });

    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);

    response.status = Status::Code(status);
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
        if (request.headers.find(name) == request.headers.end()) {
            request.headers[name] = def;
        }
    };
    fill("User-Agent", "MiniFetch");
    fill("Host", request.server);
    fill("Accept", "*/*");

    std::string cookieHeader = buildCookieHeader();
    if (!cookieHeader.empty()) {
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
            std::string filename = it->second;
            size_t fnat = 0;
            for (size_t i = 0; i < filename.size(); ++i) {
                if (filename[i] == '/' || filename[i] == '\\') {
                    fnat = i + 1;
                }
            }
            filename = filename.substr(fnat);
            query += "--" + request.boundary + "\r\n" + "Content-Disposition: form-data; name=\"" + urlEncode(it->first) + "\"\r\n" + "\r\n" + urlEncode(it->second) + "\r\n";
            ;
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
            query += "--" + request.boundary + "\r\n" + "Content-Disposition: form-data; name=\"" + urlEncode(it->first) + "\"; filename=\"" + filename + "\"\r\n" + "Content-Type: " + contentType + "\r\n" + "\r\n" + fileContents(it->second) + "\r\n";
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
    auto find = perSiteCookies.find(request.server);
    if (find != perSiteCookies.end()) {
        cookies = find->second;
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

        result += urlEncode(name) + "=" + urlEncode(value);
    }

    return result;
}

std::string MiniFetch::fileContents(std::string path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string MiniFetch::buildUrl(bool withServer) {
    std::string url;
    if (withServer) {
        url = request.protocol + "://" + request.server;
        if (!request.path.empty() && request.path[0] != '/') {
            url += "/";
        }
    }
    url += request.path;
    std::string query = buildQueryGet();
    if (!query.empty()) {
        url += "?" + query;
    }
    return url;
}

// store the received cookies in our jar
void MiniFetch::storeReceivedCookies(const MiniFetch::Request& rq, const MiniFetch::Response& rs) {
    auto& pss = perSiteCookies[rq.server];
    for (auto& cookie : rs.setCookies) {
        pss[cookie.first] = cookie.second;
    }
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
