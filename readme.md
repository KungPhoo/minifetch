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