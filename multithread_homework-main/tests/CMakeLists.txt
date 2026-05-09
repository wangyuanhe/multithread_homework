set(TEST_SOURCES
    test_quest_board.cpp
    test_dispatch_pool.cpp
    test_guild_registry.cpp
    test_guild_daemon.cpp
    test_trade_system.cpp
    test_expedition.cpp
    stress_test.cpp
)

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE guild_manager GTest::gtest_main)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    add_test(NAME ${test_name} COMMAND ${test_name})
    # 设置测试超时（防止死锁时卡住）
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 10)
endforeach()

# ============================================================
# 加分项测试（单独运行：ctest -L bonus）
# ============================================================
add_executable(test_bonus test_bonus.cpp)
target_link_libraries(test_bonus PRIVATE guild_manager GTest::gtest_main)
target_compile_options(test_bonus PRIVATE -Wall -Wextra)
add_test(NAME test_bonus COMMAND test_bonus)
set_tests_properties(test_bonus PROPERTIES
    LABELS "bonus"       # 打标签，可用 ctest -L bonus 单独运行
)
