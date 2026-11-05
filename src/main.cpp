
#if MINIFETCH_BUILD_TEST
    #include <iostream>
    #include "minifetch.h"

int main(int argc, char** argv) {
    MiniFetch mf;

    auto& rq = mf.request;
    rq.fillServerFromUrl("https://postman-echo.com/post");
    // equals:
    // rq.clear();
    // rq.method = "POST";
    // rq.protocol = "https";
    // rq.server = "postman-echo.com";
    // rq.path = "/post";
    rq.postVariables["test"] = "TestData";  // adding post variables sets rq.method from "GET" to "POST" in fetch()

    auto response = mf.fetch();

    std::cout << "Response: " << response.statusString() << std::endl;
    std::cout << response.toString() << std::endl;

    return 0;
}

#endif
