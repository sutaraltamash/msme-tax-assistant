cmake_minimum_required(VERSION 3.15)
project(tax_assistant LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Defines BUILD_TESTING (default ON) and wires ctest.
include(CTest)

set(BACKEND_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backend/include)
set(FRONTEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/frontend)

# Compiler warnings, tuned per toolchain.
set(WARNING_FLAGS -Wall -Wextra)
if(MSVC)
  set(WARNING_FLAGS /W4)
endif()

# ---------------------------------------------------------------------------
# tax_core — the backend library, compiled ONCE and shared by the server and
# every unit test. Holds the deterministic integer-cent core (Steps C-E), the
# extractor seam + orchestration (Step F), and the SQLite amalgamation.
# ---------------------------------------------------------------------------
add_library(tax_core STATIC
  backend/src/StateMatrix.cpp
  backend/src/RuleEngine.cpp
  backend/src/GSTRGenerator.cpp
  backend/src/MockExtractor.cpp
  backend/src/GeminiClient.cpp
  backend/src/InvoiceRepository.cpp
  backend/src/Pipeline.cpp
  backend/include/sqlite3.c         # SQLite amalgamation compiled as C
)

# The HTTPS transport for the probabilistic layer (HttpClient seam):
# Windows -> WinHTTP (Schannel TLS, no OpenSSL/libcurl on this toolchain);
# everything else -> libcurl.
if(WIN32)
  target_sources(tax_core PRIVATE backend/src/WinHttpClient.cpp)
  target_link_libraries(tax_core PRIVATE winhttp)
else()
  find_package(CURL REQUIRED)
  target_sources(tax_core PRIVATE backend/src/CurlHttpClient.cpp)
  target_link_libraries(tax_core PRIVATE CURL::libcurl)
endif()

# crow.h/crow/*.h resolve via -I backend/include ("crow/..." style includes);
# standalone Asio resolves via <asio.hpp> from -I backend/include/asio. Public
# so the server and every test executable inherit the same vendored headers.
target_include_directories(tax_core PUBLIC
  ${BACKEND_INCLUDE_DIR}
  ${BACKEND_INCLUDE_DIR}/asio
)

# Platform toolchain flags every C++ TU in this project needs (used by the
# vendored standalone Asio / Crow): Windows-first per project decision.
if(WIN32)
  # _WIN32_WINNT=0x0601 targets Windows 7+ APIs (WSAPoll etc.);
  # WIN32_LEAN_AND_MEAN strips winsock.h, forcing Asio onto winsock2.h.
  target_compile_definitions(tax_core PUBLIC _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  target_compile_definitions(tax_core PUBLIC NOMINMAX)
endif()

target_compile_options(tax_core PUBLIC ${WARNING_FLAGS})

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(tax_assistant_server
  backend/src/main.cpp
)

target_link_libraries(tax_assistant_server PRIVATE tax_core)

if(WIN32)
  # Static-link into the WinSock subsystem. pthread does not exist on native
  # MSVC/Windows, so it is never linked here. winhttp has already been attached
  # to tax_core for the HttpClient transport.
  target_link_libraries(tax_assistant_server PRIVATE ws2_32 crypt32 wsock32)
else()
  target_link_libraries(tax_assistant_server PRIVATE pthread)
endif()

# --- Frontend → build-tree copy (static assets served by Crow) ------------
add_custom_command(TARGET tax_assistant_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${FRONTEND_DIR} $<TARGET_FILE_DIR:tax_assistant_server>/frontend
  COMMENT "Copying frontend static assets into build output"
)

# ---------------------------------------------------------------------------
# Tests (Phase 1 gate)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()