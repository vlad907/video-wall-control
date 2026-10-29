cmake_minimum_required(VERSION 3.24)
project(MosaicEngine VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Third-party (header-only, pinned)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.26.0
  GIT_SHALLOW TRUE)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(HTTPLIB_COMPILE OFF CACHE INTERNAL "")
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE INTERNAL "")
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE INTERNAL "")
# stb has no CMakeLists.txt, so MakeAvailable only downloads it.
FetchContent_MakeAvailable(nlohmann_json httplib stb)

# ---------------------------------------------------------------------------
# NDI SDK (optional). Point NDI_SDK_DIR at the SDK install folder.
#   Windows default: C:/Program Files/NDI/NDI 6 SDK
#   macOS default:   /Library/NDI SDK for Apple
# ---------------------------------------------------------------------------
if(WIN32)
  set(_ndi_default "C:/Program Files/NDI/NDI 6 SDK")
elseif(APPLE)
  set(_ndi_default "/Library/NDI SDK for Apple")
else()
  set(_ndi_default "/usr/local/ndi-sdk")
endif()
set(NDI_SDK_DIR "${_ndi_default}" CACHE PATH "NDI SDK root")

find_path(NDI_INCLUDE_DIR Processing.NDI.Lib.h PATHS "${NDI_SDK_DIR}/include" "${NDI_SDK_DIR}/Include" NO_DEFAULT_PATH)
if(WIN32)
  find_library(NDI_LIBRARY NAMES Processing.NDI.Lib.x64 PATHS "${NDI_SDK_DIR}/Lib/x64" NO_DEFAULT_PATH)
else()
  find_library(NDI_LIBRARY NAMES ndi ndi_advanced PATHS "${NDI_SDK_DIR}/lib/macOS" "${NDI_SDK_DIR}/lib/x86_64-linux-gnu" "${NDI_SDK_DIR}/lib" NO_DEFAULT_PATH)
endif()
if(NDI_INCLUDE_DIR AND NDI_LIBRARY)
  set(MOSAIC_HAVE_NDI ON)
  message(STATUS "NDI SDK found: ${NDI_LIBRARY}")
else()
  set(MOSAIC_HAVE_NDI OFF)
  message(WARNING "NDI SDK not found under '${NDI_SDK_DIR}'. Building without NDI (outputs go to a measuring null sink). See docs/NDI.md.")
endif()

# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------
set(ENGINE_SOURCES
  engine/src/main.cpp
  engine/src/core/layout.cpp
  engine/src/core/layout_store.cpp
  engine/src/core/engine.cpp
  engine/src/core/output_channel.cpp
  engine/src/io/null_sink.cpp
  engine/src/api/http_api.cpp
  engine/src/util/image.cpp
)
if(MOSAIC_HAVE_NDI)
  list(APPEND ENGINE_SOURCES engine/src/io/ndi_sdk.cpp)
else()
  list(APPEND ENGINE_SOURCES engine/src/io/ndi_none.cpp)
endif()

if(APPLE)
  list(APPEND ENGINE_SOURCES engine/src/render/metal/metal_renderer.mm)
elseif(WIN32)
  list(APPEND ENGINE_SOURCES engine/src/render/d3d11/d3d11_renderer.cpp)
else()
  message(FATAL_ERROR "Only Windows (Direct3D 11) and macOS (Metal) backends are implemented.")
endif()

add_executable(mosaic_engine ${ENGINE_SOURCES})
target_include_directories(mosaic_engine PRIVATE engine/src ${stb_SOURCE_DIR})
target_link_libraries(mosaic_engine PRIVATE nlohmann_json::nlohmann_json httplib::httplib)
target_compile_definitions(mosaic_engine PRIVATE MOSAIC_VERSION="${PROJECT_VERSION}")

if(MOSAIC_HAVE_NDI)
  target_include_directories(mosaic_engine PRIVATE ${NDI_INCLUDE_DIR})
  target_link_libraries(mosaic_engine PRIVATE ${NDI_LIBRARY})
  target_compile_definitions(mosaic_engine PRIVATE MOSAIC_HAVE_NDI=1)
  if(WIN32)
    # Copy the NDI runtime next to the exe.
    file(GLOB _ndi_dll "${NDI_SDK_DIR}/Bin/x64/Processing.NDI.Lib.x64.dll")
    if(_ndi_dll)
      add_custom_command(TARGET mosaic_engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ndi_dll} $<TARGET_FILE_DIR:mosaic_engine>)
    endif()
  endif()
endif()

if(APPLE)
  set_source_files_properties(engine/src/render/metal/metal_renderer.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(mosaic_engine PRIVATE "-framework Metal" "-framework Foundation" "-framework IOKit" "-framework CoreFoundation" "-framework CFNetwork")
elseif(WIN32)
  target_compile_definitions(mosaic_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(mosaic_engine PRIVATE d3d11 dxgi d3dcompiler pdh ws2_32)
  if(MSVC)
    target_compile_options(mosaic_engine PRIVATE /W3 /utf-8 /permissive-)
  endif()
endif()

if(NOT MSVC)
  target_compile_options(mosaic_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
