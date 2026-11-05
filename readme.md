# MiniFetch
A library for getting web content with very small footprint
and dependencies.

## Features
- Uses winhttp on Windows
- Uses libcurl on Linux/Mac
- Uses asyncronous emscripten_fetch for EMSCRIPTEN
- can use http and https
- GET and POST data

## Add and build with CMake
To clone as a submodule just type:
```
git submodule add -- https://github.com/KungPhoo/minifetch.git minifetch
```

In your CMakeLists.txt just add:
```
add_subdirectory(minifetch) # where you cloned this submodule
target_link_libraries(${APP_NAME} PRIVATE minifetch)
```
## Requests and concurrent use (C++17)

```cpp
MiniFetch client;
MiniFetch::Request request;
if (!request.fillServerFromUrl("https://example.com/api")) {
    // Handle invalid URL.
    return;
}
request.headers["Content-Type"] = "application/json";
request.rawPostData = R"({"hello":"world"})"; // Infers POST for a default GET.
auto response = client.fetch(request);
if (!response.error.empty()) {
    // Transport/setup failure. HTTP errors such as 404 instead have status/body.
}
```

Share the client between threads and give each caller its own Request. Networking
runs concurrently; only cookie state is locked. `cookieJar()` returns a locked
snapshot and `clearCookies()` clears native cookies. The legacy public `request`
plus `fetch()` API requires external synchronization if you mutate it across
threads. Fetch no longer modifies the supplied request, and clients are
noncopyable. C++20 applications can use the same API.

Emscripten uses the same `client.fetch(request)` or `client.fetch()` API.
Internally it starts an asynchronous Fetch and yields with Asyncify until the
callback completes. The CMake target propagates `-sFETCH=1 -sASYNCIFY=1`; use both
flags when linking without CMake. Call from an Asyncify-capable application entry
point. Do not start another suspended fetch on the same browser thread while one
is pending; Asyncify cannot suspend overlapping call stacks. Browser-controlled
cookies, CORS, and redirects still differ from native platforms. See
[the review and API notes](docs/review.md) for details.

## Standalone build and regression checks

Linux needs a C++17 compiler, CMake 3.13+, libcurl development files, and Python 3
for the optional fixture tests. No public Internet access is needed by the tests.

```sh
cmake -S . -B out/build -DMINIFETCH_BUILD_REGRESSION_TESTS=ON
cmake --build out/build --parallel 2
ctest --test-dir out/build --output-on-failure
```

Set `MINIFETCH_BUILD_TEST=OFF` to omit the public-network example. Its default is
ON for backward compatibility. When embedding the library with `add_subdirectory`,
the `minifetch` target propagates C++17 and platform link requirements. On Linux
with several curl installations, select matching headers and a library with
`CURL_INCLUDE_DIR` and `CURL_LIBRARY` as necessary.

For a browser build with an installed Emscripten SDK:

```sh
emcmake cmake -S . -B out/wasm
cmake --build out/wasm --parallel 2
```

The target propagates Fetch, Asyncify, and C++ exception handling. The public
fetch API is the same on native and browser builds.
