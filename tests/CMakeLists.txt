add_executable(test_format test_format.c)
target_link_libraries(test_format PRIVATE hx_clog::hx_clog)
add_test(NAME test_format COMMAND test_format)

add_executable(test_rotate test_rotate.c)
target_link_libraries(test_rotate PRIVATE hx_clog::hx_clog)
add_test(NAME test_rotate COMMAND test_rotate)

add_executable(test_largeline test_largeline.c)
target_link_libraries(test_largeline PRIVATE hx_clog::hx_clog)
add_test(NAME test_largeline COMMAND test_largeline)

add_executable(test_crash test_crash.c)
target_link_libraries(test_crash PRIVATE hx_clog::hx_clog)
add_test(NAME test_crash COMMAND test_crash)

# Opt-in: actually trigger a fault to exercise the crash report path.
option(HX_CLOG_TEST_TRIGGER_CRASH "Make test_crash trigger a real crash" OFF)
if(HX_CLOG_TEST_TRIGGER_CRASH)
    target_compile_definitions(test_crash PRIVATE HX_CLOG_TEST_TRIGGER_CRASH=1)
endif()

if(HX_CLOG_ENABLE_ASYNC)
    add_executable(test_async test_async.c)
    target_link_libraries(test_async PRIVATE hx_clog::hx_clog)
    add_test(NAME test_async COMMAND test_async)
endif()
