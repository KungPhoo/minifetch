#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>
#include <utility>

class MiniFetch {
public:
    struct Status {
        enum Code : int {
            InternalError               = -1,
            Continue                    = 100,
            SwitchingProtocols          = 101,
            Processing                  = 102,
            OK                          = 200,
            Created                     = 201,
            Accepted                    = 202,
            NonAuthoritativeInfo        = 203,
            NoContent                   = 204,
            ResetContent                = 205,
            PartialContent              = 206,
            MultiStatus                 = 207,
            AlreadyReported             = 208,
            IMUsed                      = 226,
            MultipleChoices             = 300,
            MovedPermanently            = 301,
            Found                       = 302,
            SeeOther                    = 303,
            NotModified                 = 304,
            UseProxy                    = 305,
            SwitchProxy                 = 306,
            Temp                        = 307,
            Permanent                   = 308,
            BadRequest                  = 400,
            Unauthorized                = 401,
            PaymentRequired             = 402,
            Forbidden                   = 403,
            NotFound                    = 404,
            MethodNotAllowed            = 405,
            NotAcceptable               = 406,
            ProxyAuthRequired           = 407,
            RequestTimeout              = 408,
            Conflict                    = 409,
            Gone                        = 410,
            LengthRequired              = 411,
            PreconditionFailed          = 412,
            PayloadTooLarge             = 413,
            URITooLong                  = 414,
            UnsupportedMediaType        = 415,
            RangeNotSatisfiable         = 416,
            ExpectationFailed           = 417,
            ImATeapot                   = 418,
            MisdirectedRequest          = 421,
            UnprocessableEntity         = 422,
            Locked                      = 423,
            FailedDependency            = 424,
            UpgradeRequired             = 426,
            PreconditionRequired        = 428,
            TooManyRequests             = 429,
            RequestHeaderFieldsTooLarge = 431,
            LoginTimeOut                = 440,
            RetryWith                   = 449,
            UnavailableForLegalReasons  = 451,
            Internal                    = 500,
            NotImplemented              = 501,
            BadGateway                  = 502,
            ServiceUnavailable          = 503,
            GatewayTimeout              = 504,
            HTTPVersionNotSupported     = 505,
            VariantAlsoNegotiates       = 506,
            InsufficientStorage         = 507,
            LoopDetected                = 508,
            BandwidthLimitExceeded      = 509,
            NotExtended                 = 510,
            NetworkAuthRequired         = 511
        };
    };

    // request input structure. Fill this before calling fetch().
    struct Request {
        std::string protocol = "https";
        std::string server;
        std::string path   = "/";
        std::string method = "GET"; // "GET" is set to "POST" if postVariables is not empty.
        std::map<std::string, std::string> headers;
        std::map<std::string, std::string> getVariables;
        std::map<std::string, std::string> postVariables;
        std::map<std::string, std::string> postFileNames; // [varname] = filepath. upload content of these files
        std::map<std::string, std::string> cookies; // adds or overwrites stored cookies

        std::string rawPostData; // optional, additional data (JSON...), that's appended

        std::string port     = ""; // optional
        int timeoutSeconds   = 30; // 0 means no timeout.
        bool withCredentials = true; // Browser-managed cookies (Emscripten only).
        std::string boundary = "xxxMiniFetchFormBoundary7Mx4YZxkTrZu0gxxx";

        void clear() {
            *this = { };
        }
        bool fillServerFromUrl(std::string url = "https://www.example.com/dir/"); // clears all other variables
    } request = { }; // Legacy API: externally synchronize edits and fetch().

    // response information and parser
    struct Response {
        Status::Code status = Status::InternalError;
        std::vector<uint8_t> bytes;
        std::map<std::string, std::string> setCookies;
        std::vector<std::string> setCookieHeaders; // Preserves attributes and repeated cookie names.
        std::string error; // Transport/setup failure; HTTP errors still have a status/body.
        std::string location; // in case of 302 ("Found" redirect)
        std::string toString() const {
            std::string result(bytes.begin(), bytes.end());
            return result;
        }
        std::string statusString() const;
    };

    // the actual fetch method
    MiniFetch::Response fetch();
    // Concurrent calls are safe when each caller owns its Request. No network-wide lock.
    MiniFetch::Response fetch(Request request);
#ifdef __EMSCRIPTEN__
    // Browser-main-thread friendly. Setup errors call back inline, network completion
    // on the event loop. Callbacks must not throw; destroying the client is safe.
    void fetchAsync(Request request, std::function<void(Response)> callback);
#endif
    MiniFetch() = default;
    MiniFetch(const MiniFetch&) = delete;
    MiniFetch& operator=(const MiniFetch&) = delete;

    static std::string urlEncode(const std::string& value);

    // just for debugging
    std::map<std::string, std::map<std::string, std::string>> cookieJar() const;
    void clearCookies();

private:
    struct CookieState {
        std::mutex mutex;
        struct Cookie {
            std::string value;
            bool secure = false;
            std::optional<std::chrono::system_clock::time_point> expires;
        };
        // Exact origin -> (path, name) -> cookie. Deliberately never share across origins.
        std::map<std::string, std::map<std::pair<std::string, std::string>, Cookie>> values;
    };
    std::shared_ptr<CookieState> cookies_ = std::make_shared<CookieState>();
    explicit MiniFetch(std::shared_ptr<CookieState> state) : cookies_(std::move(state)) {}
    Response fetchImpl();
    bool prepare(Response& response, std::string& body);
    static std::string cookieOrigin(const Request& request);
    std::string buildQueryPost();
    std::string buildQueryGet();
    std::map<std::string, std::string> getRequestCookies() const;
    std::string buildCookieHeader() const;
    std::string fileContents(std::string path);
    std::string buildUrl(bool withServer = true);

    void storeReceivedCookies(const Request& rq, const Response& rs);
};
