# CMakeList.txt : CMake project for minifetch, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.12)

# include this as a submodule, then write add_subdirectory(minifetch) and target_link_libraries(myapp PRIVATE minifetch)

set(MINIFETCH_BUILD_TEST "DEFAULT" CACHE STRING "Want to build the minifetch test app? YES, NO")
set_property(CACHE MINIFETCH_BUILD_TEST PROPERTY STRINGS "YES" "NO")
if(${MINIFETCH_BUILD_TEST} STREQUAL "YES" )
add_compile_definitions(MINIFETCH_BUILD_TEST)
else()
add_compile_definitions(MINIFETCH_BUILD_TEST)
endif()



# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("minifetch")

add_library(minifetch STATIC src/minifetch.cpp src/minifetch.h)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (MSVC)
    # this one fixes intellisense
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++20 /Zc:__cplusplus")
endif()

target_include_directories(minifetch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")


if(NOT WIN32 AND NOT EMSCRIPTEN)
    #sudo apt-get install libcurl4-openssl-dev
    find_package(CURL REQUIRED)
    add_definitions(-DUSE_LIBCURL)
    target_link_libraries(minifetch PRIVATE CURL::libcurl)
endif()


# Add source to this project's executable.
add_executable (minifetchtest src/main.cpp)
target_link_libraries(minifetchtest PRIVATE minifetch)

