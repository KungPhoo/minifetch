#if MINIFETCH_BUILD_TEST
#include <iostream>
#include <utility>
#include "minifetch.h"

int main() {
    MiniFetch client;
    MiniFetch::Request request;
    if (!request.fillServerFromUrl("https://postman-echo.com/post")) return 1;
    request.postVariables["test"] = "TestData";
    auto print = [](MiniFetch::Response response) {
        if (!response.error.empty()) std::cerr << response.error << '\n';
        else std::cout << "Response: " << response.statusString() << '\n' << response.toString() << '\n';
    };
    auto response = client.fetch(std::move(request));
    const bool failed = !response.error.empty();
    print(std::move(response));
    if (failed) return 1;
    return 0;
}
#endif
