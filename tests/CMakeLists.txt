add_executable(test_format test_format.c)
target_link_libraries(test_format PRIVATE hx_clog::hx_clog)
add_test(NAME test_format COMMAND test_format)

add_executable(test_rotate test_rotate.c)
target_link_libraries(test_rotate PRIVATE hx_clog::hx_clog)
add_test(NAME test_rotate COMMAND test_rotate)

add_executable(test_largeline test_largeline.c)
target_link_libraries(test_largeline PRIVATE hx_clog::hx_clog)
add_test(NAME test_largeline COMMAND test_largeline)

add_executable(test_features test_features.c)
target_link_libraries(test_features PRIVATE hx_clog::hx_clog)
add_test(NAME test_features COMMAND test_features)

add_executable(test_rotate_time test_rotate_time.c)
target_link_libraries(test_rotate_time PRIVATE hx_clog::hx_clog)
add_test(NAME test_rotate_time COMMAND test_rotate_time)

add_executable(test_utf8path test_utf8path.c)
target_link_libraries(test_utf8path PRIVATE hx_clog::hx_clog)
add_test(NAME test_utf8path COMMAND test_utf8path)

add_executable(test_date_subdir test_date_subdir.c)
target_link_libraries(test_date_subdir PRIVATE hx_clog::hx_clog)
add_test(NAME test_date_subdir COMMAND test_date_subdir)

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

if(HX_CLOG_ENABLE_CPP11)
    add_executable(test_cpp11 test_cpp11.cpp)
    target_link_libraries(test_cpp11 PRIVATE hx_clog::hx_clog)
    add_test(NAME test_cpp11 COMMAND test_cpp11)
endif()

# Shared builds on Windows: the test executables need hx_clog.dll next to
# them (there is no rpath equivalent), so copy it after each build.
if(WIN32 AND HX_CLOG_BUILD_SHARED)
    set(_hx_test_targets
        test_format test_rotate test_largeline test_features
        test_rotate_time test_utf8path test_date_subdir test_crash)
    if(HX_CLOG_ENABLE_ASYNC)
        list(APPEND _hx_test_targets test_async)
    endif()
    if(HX_CLOG_ENABLE_CPP11)
        list(APPEND _hx_test_targets test_cpp11)
    endif()
    foreach(_t IN LISTS _hx_test_targets)
        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:hx_clog> $<TARGET_FILE_DIR:${_t}>)
    endforeach()
endif()
