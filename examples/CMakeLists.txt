add_executable(c_basic c_basic.c)
target_link_libraries(c_basic PRIVATE hx_clog::hx_clog)

add_executable(c_rotate c_rotate.c)
target_link_libraries(c_rotate PRIVATE hx_clog::hx_clog)

if(HX_CLOG_ENABLE_ASYNC)
    add_executable(c_async c_async.c)
    target_link_libraries(c_async PRIVATE hx_clog::hx_clog)
endif()

if(HX_CLOG_ENABLE_CPP11)
    add_executable(cpp11_basic cpp11_basic.cpp)
    target_link_libraries(cpp11_basic PRIVATE hx_clog::hx_clog)
endif()

# Shared builds on Windows: examples need hx_clog.dll next to the exe.
if(WIN32 AND HX_CLOG_BUILD_SHARED)
    set(_hx_example_targets c_basic c_rotate)
    if(HX_CLOG_ENABLE_ASYNC)
        list(APPEND _hx_example_targets c_async)
    endif()
    if(HX_CLOG_ENABLE_CPP11)
        list(APPEND _hx_example_targets cpp11_basic)
    endif()
    foreach(_t IN LISTS _hx_example_targets)
        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:hx_clog> $<TARGET_FILE_DIR:${_t}>)
    endforeach()
endif()
