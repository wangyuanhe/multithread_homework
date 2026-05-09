cmake_minimum_required(VERSION 3.16)
project(GuildManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# 编译选项
# ============================================================
option(ENABLE_TSAN "Enable ThreadSanitizer (检测数据竞争)" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer (检测内存错误)" OFF)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)

if(ENABLE_TSAN AND ENABLE_ASAN)
    message(FATAL_ERROR "TSAN 和 ASAN 不能同时开启")
endif()

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer 已启用 — 可检测数据竞争")
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer 已启用 — 可检测内存错误")
    add_compile_options(-fsanitize=address -g -O1)
    add_link_options(-fsanitize=address)
endif()

# ============================================================
# 主库：guild_manager
# ============================================================
add_library(guild_manager
    src/dispatch_pool.cpp
    src/guild_registry.cpp
    src/guild_daemon.cpp
    src/trade_system.cpp
    src/expedition.cpp
)

target_include_directories(guild_manager PUBLIC include)
target_link_libraries(guild_manager PUBLIC pthread)
target_compile_options(guild_manager PRIVATE -Wall -Wextra -Wpedantic)

# ============================================================
# 测试
# ============================================================
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(third_party)
    add_subdirectory(tests)
endif()

# ============================================================
# 基准测试
# ============================================================
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
