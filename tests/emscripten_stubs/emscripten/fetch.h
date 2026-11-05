#pragma once
// Minimal test double for the Emscripten 4.0.19 Fetch API, not a browser emulator.
#include <cstddef>
#include <cstdint>
#define EMSCRIPTEN_FETCH_LOAD_TO_MEMORY 1
#define EMSCRIPTEN_FETCH_REPLACE 16
#define EMSCRIPTEN_FETCH_SYNCHRONOUS 64
struct emscripten_fetch_t;
struct emscripten_fetch_attr_t {
    char requestMethod[32];
    void* userData;
    void (*onsuccess)(emscripten_fetch_t*);
    void (*onerror)(emscripten_fetch_t*);
    const char* const* requestHeaders;
    const char* requestData;
    std::size_t requestDataSize;
    uint32_t attributes;
    uint32_t timeoutMSecs;
    bool withCredentials;
};
struct emscripten_fetch_t {
    void* userData;
    const char* data;
    uint64_t numBytes;
    unsigned short status;
};
extern "C" {
void emscripten_fetch_attr_init(emscripten_fetch_attr_t*);
emscripten_fetch_t* emscripten_fetch(emscripten_fetch_attr_t*, const char*);
int emscripten_fetch_close(emscripten_fetch_t*);
std::size_t emscripten_fetch_get_response_headers_length(emscripten_fetch_t*);
std::size_t emscripten_fetch_get_response_headers(emscripten_fetch_t*, char*, std::size_t);
}
