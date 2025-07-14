#pragma once
#include <string>
#include <vector>
#include <map>

class MiniFetch {
    public:
    struct Status {
        enum Code {
            InternalError = -1,
            Continue = 100,
            SwitchingProtocols = 101,
            Processing = 102,
            OK = 200,
            Created = 201,
            Accepted = 202,
            NonAuthoritativeInfo = 203,
            NoContent = 204,
            ResetContent = 205,
            PartialContent = 206,
            MultiStatus = 207,
            AlreadyReported = 208,
            IMUsed = 229,
            MultipleChoices = 300,
            MovedPermanently = 301,
            Found = 302,
            SeeOther = 303,
            NotModified = 304,
            UseProxy = 305,
            SwitchProxy = 306,
            Temp = 307,
            Permanent = 308,
            BadRequest = 400,
            Unauthorized = 401,
            PaymentRequired = 402,
            Forbidden = 403,
            NotFound = 404,
            MethodNotAllowed = 405,
            NotAcceptable = 406,
            ProxyAuthRequired = 407,
            RequestTimeout = 408,
            Conflict = 409,
            Gone = 410,
            LengthRequired = 411,
            PreconditionFailed = 412,
            PayloadTooLarge = 413,
            URITooLong = 414,
            UnsupportedMediaType = 415,
            RangeNotSatisfiable = 416,
            ExpectationFailed = 417,
            ImATeapot = 418,
            MisdirectedRequest = 421,
            UnprocessableEntity = 422,
            Locked = 423,
            FailedDependency = 424,
            UpgradeRequired = 426,
            PreconditionRequired = 428,
            TooManyRequests = 429,
            RequestHeaderFieldsTooLarge = 431,
            LoginTimeOut = 440,
            RetryWith = 449,
            UnavailableForLegalReasons = 451,
            InternalServer = 500,
            NotImplemented = 501,
            BadGateway = 502,
            ServiceUnavailable = 503,
            GatewayTimeout = 504,
            HTTPVersionNotSupported = 505,
            VariantAlsoNegotiates = 506,
            InsufficientStorage = 507,
            LoopDetected = 508,
            BandwidthLimitExceeded = 509,
            NotExtended = 510,
            NetworkAuthRequired = 511
        };
    };

    // request input structure. Fill this before calling fetch().
    struct Request {
        std::string protocol = "https";
        std::string server;
        std::string path = "/";
        std::string method = "POST";
        std::map<std::string, std::string> headers;
        std::map<std::string, std::string> getVariables;
        std::map<std::string, std::string> postVariables;
        std::map<std::string, std::string> postFileNames;  // [varname] = filepath. upload content of these files
        std::string port = "";                             // optional
        int timeoutSeconds = 30;
        std::string boundary = "xxxMiniFetchFormBoundary7Mx4YZxkTrZu0gxxx";

        bool fillServerFromUrl(std::string url = "https://www.example.com/dir/");
    } request = {};

    // response information and parser
    struct Response {
        Status::Code status = Status::InternalError;
        std::vector<uint8_t> bytes;
        std::string toString() const {
            std::string result(bytes.begin(), bytes.end());
            return result;
        }
        std::string statusString() const;
    };

    // the actual fetch method
    MiniFetch::Response fetch();

    static std::string urlEncode(const std::string& value);

    private:
    void prepareHeaders();
    std::string buildQueryPost();
    std::string buildQueryGet();
    std::string fileContents(std::string path);
    std::string buildUrl(bool withServer = true);
};