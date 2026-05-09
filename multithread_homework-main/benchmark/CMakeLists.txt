add_executable(bench_guild bench_guild.cpp)
target_link_libraries(bench_guild PRIVATE guild_manager)
target_compile_options(bench_guild PRIVATE -O2)
