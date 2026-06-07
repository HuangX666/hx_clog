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
