// minifetch.cpp : lightweight web fetch library
#include "minifetch.h"
#include <sstream>
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

MiniFetch::Response MiniFetch::fetch() {
    MiniFetch::Response response = {};
#if defined(_WIN32)

    // --
    auto from_bytes = [](const std::string& input) -> std::wstring {
        if (input.empty()) return std::wstring();
        int wide_len = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
        if (wide_len == 0) {
            return std::wstring();
        }
        std::wstring result(wide_len, 0);
        MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, &result[0], wide_len);
        // Remove the null terminator that was included by MultiByteToWideChar
        result.pop_back();
        return result;
    };

    std::wstring wServer = from_bytes(request.server);
    INTERNET_PORT port = request.protocol == "https" ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (!request.port.empty()) port = std::stoi(request.port);

    std::string url = buildUrl();
    std::wstring wPath = from_bytes(url);

    HINTERNET hSession = WinHttpOpen(L"WinHTTP Client/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    HINTERNET hConnect = WinHttpConnect(hSession, wServer.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD flags = (request.protocol == "https") ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                            from_bytes(request.method).c_str(),
                                            wPath.c_str(),
                                            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Set headers
    for (auto& [k, v] : request.headers) {
        std::wstring headerLine = from_bytes(k + ": " + v + "\r\n");
        WinHttpAddRequestHeaders(hRequest, headerLine.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    std::string postData = buildQuery(request.postVariables);
    BOOL bResults = WinHttpSendRequest(hRequest,
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

    bResults = WinHttpReceiveResponse(hRequest, NULL);
    if (!bResults) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    do {
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;

        std::vector<uint8_t> buffer(dwSize);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) break;

        response.bytes.insert(response.bytes.end(), buffer.begin(), buffer.begin() + dwDownloaded);
    } while (dwSize > 0);

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    response.status = StatusCode(statusCode);
    return response;
#elif defined(__EMSCRIPTEN__)
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, request.method.c_str());

    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY |
                      EMSCRIPTEN_FETCH_SYNCHRONOUS;  // Synchronous!
    std::string fullUrl = buildUrl();
    emscripten_fetch_t* fetch = emscripten_fetch(&attr, fullUrl);

    if (fetch->status == 200) {
        response.bytes.assign(fetch->data, fetch->data + fetch->numBytes);
    }

    int status = fetch->status;
    emscripten_fetch_close(fetch);
    response.status = StatusCode(status);
    return response;

#else
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string url = buildUrl();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeoutSeconds);

    struct curl_slist* headers = nullptr;
    for (auto& [k, v] : request.headers) {
        std::string line = k + ": " + v;
        headers = curl_slist_append(headers, line.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    std::string postData = buildQuery(request.postVariables);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* out = static_cast<std::vector<uint8_t>*>(userdata);
            size_t total = size * nmemb;
            out->insert(out->end(), (uint8_t*)ptr, (uint8_t*)ptr + total);
            return total; });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.bytes);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    response.status = StatusCode(status);
    return response;
#endif
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

std::string MiniFetch::buildQuery(const std::map<std::string, std::string>& vars) {
    std::string query;
    for (auto it = vars.begin(); it != vars.end(); ++it) {
        if (it != vars.begin()) query += "&";
        query += urlEncode(it->first) + "=" + urlEncode(it->second);
    }
    return query;
}

std::string MiniFetch::buildUrl() {
    std::string url = request.protocol + "://" + request.server;
    if (!request.path.empty() && request.path[0] != '/') {
        url += "/";
    }
    url += request.path;
    std::string query = buildQuery(request.getVariables);
    if (!query.empty()) {
        url += "?" + query;
    }
    return url;
}

std::string MiniFetch::Response::statusString() const {
#define MCASE(a) \
    case a:      \
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
