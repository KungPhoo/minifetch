#include "minifetch.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
MiniFetch::Request request(const std::string& url) {
    MiniFetch::Request result;
    if (!result.fillServerFromUrl(url)) throw std::runtime_error("URL parsing failed");
    result.timeoutSeconds = 15;
    return result;
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "fixture URLs missing");
        const std::string base = argv[1], other = argv[2];
        MiniFetch client;
        auto rq = request(base + "/method");
        auto response = client.fetch(rq);
        require(response.error.empty() && response.status == 200 && response.toString() == "GET", "empty GET");
        require(rq.method == "GET" && rq.headers.empty(), "caller Request was modified");
        rq = request(base + "/echo");
        rq.postVariables = {{"a", "hello & world"}};
        response = client.fetch(rq);
        require(response.status == 200 && response.toString() == "a=hello%20%26%20world", "form POST body");
        rq = request(base + "/echo");
        rq.rawPostData = std::string("a\0b", 3);
        response = client.fetch(rq);
        require(response.status == 200 && response.toString() == rq.rawPostData, "binary raw POST body");
        rq.method = "PUT";
        require(client.fetch(rq).toString() == rq.rawPostData, "PUT body");
        rq = request(base + "/method"); rq.method = "POST";
        require(client.fetch(rq).toString() == "POST", "empty POST");
        rq.method = "HEAD";
        response = client.fetch(rq);
        require(response.status == 200 && response.bytes.empty(), "HEAD request");
        rq = request(base + "/query?existing=1#fragment"); rq.getVariables["a"] = "b c";
        require(client.fetch(rq).toString() == "/query?existing=1&a=b%20c", "existing query or fragment");
        rq = request(base + "/header"); rq.headers["X-Test"] = "custom";
        require(client.fetch(rq).toString() == "custom", "custom header");
        response = client.fetch(request(base + "/redirect"));
        require(response.status == 302 && response.location == "/target" && response.toString() == "redirect-body", "manual redirect");
        response = client.fetch(request(base + "/error"));
        require(response.status == 418 && response.error.empty() && response.toString() == "error-body", "HTTP error body");
        response = client.fetch(request(base + "/status599"));
        require(response.status == 599 && response.statusString() == "Status599", "nonstandard HTTP status");
        client.fetch(request(base + "/set"));
        const auto cookies = client.fetch(request(base + "/cookie")).toString();
        require(cookies == "session=a%2Fb", "cookie encoding, Secure, expiry or path scoping");
        require(client.fetch(request(base + "/private/cookie")).toString() == "private=secret; session=a%2Fb", "cookie path matching");
        require(client.fetch(request(other + "/cookie")).toString().empty(), "cookie leaked to another port");
        client.fetch(request(base + "/delete"));
        require(client.fetch(request(base + "/cookie")).toString().empty(), "cookie deletion");
        client.clearCookies();
        require(client.cookieJar().empty(), "clearCookies");
        rq = request(base + "/echo"); rq.postFileNames["file"] = "missing-minifetch-regression-file";
        require(!client.fetch(rq).error.empty(), "missing file did not fail");
        const std::string upload = "minifetch-regression-upload.tmp";
        { std::ofstream file(upload, std::ios::binary); file.write("binary\0data", 11); }
        rq.postFileNames["file"] = upload; rq.postVariables["text"] = "hello & world";
        response = client.fetch(rq);
        std::remove(upload.c_str());
        require(response.status == 200 && response.toString().find("\r\n\r\nhello & world\r\n") != std::string::npos &&
            response.toString().find(std::string("binary\0data", 11)) != std::string::npos &&
            response.toString().find("filename=\"minifetch-regression-upload.tmp\"") != std::string::npos, "multipart encoding");
        rq = request(base + "/"); rq.headers["X-Bad"] = "injected\r\nHeader: value";
        require(!client.fetch(rq).error.empty(), "header injection accepted");
        rq = request(base + "/"); rq.headers["Content-Length"] = "999";
        require(!client.fetch(rq).error.empty(), "caller framing accepted");
        rq = request(base + "/"); rq.port = "70000";
        require(!client.fetch(rq).error.empty(), "invalid port accepted");
        MiniFetch::Request parsed;
        require(parsed.fillServerFromUrl("https://example.com?x=1#frag") && parsed.server == "example.com" && parsed.path == "/?x=1", "query-only URL");
        require(parsed.fillServerFromUrl("http://[::1]:8080/path") && parsed.server == "[::1]" && parsed.port == "8080", "IPv6 URL");
        require(!parsed.fillServerFromUrl("file:///etc/passwd") && !parsed.fillServerFromUrl("http://user@host/"), "invalid URL accepted");
        // The fixture barrier requires all eight requests to be in flight together.
        std::atomic<int> passed{0};
        std::atomic<bool> finished{false};
        std::thread observer([&] { while (!finished.load()) { client.cookieJar(); std::this_thread::yield(); } });
        std::vector<std::thread> workers;
        for (int i = 0; i != 8; ++i) workers.emplace_back([&, i] {
            auto result = client.fetch(request(base + "/parallel?id=" + std::to_string(i)));
            if (result.error.empty() && result.status == 200) ++passed;
        });
        for (auto& worker : workers) worker.join();
        finished = true; observer.join();
        require(passed == 8, "parallel requests failed or serialized");
        const auto jar = client.cookieJar();
        require(jar.size() == 1 && jar.begin()->second.size() == 8, "concurrent cookie writes lost");
        // Closed local port: transport failure must differ from an HTTP response.
        auto failed = request("http://127.0.0.1:1/"); failed.timeoutSeconds = 1;
        response = client.fetch(failed);
        require(response.status == MiniFetch::Status::InternalError && !response.error.empty(), "transport failure status");
        std::cout << checks << " regression checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
