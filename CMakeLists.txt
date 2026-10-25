cmake_minimum_required(VERSION 3.22.1)
project(coro_lite LANGUAGES CXX)

add_library(coro_lite STATIC
        CancellationToken.cpp
        CoroTask.cpp
        Endpoint.cpp
        FdRegistry.cpp
        TimerPool.cpp
        Socket.cpp
)
add_library(CoroLite::coro_lite ALIAS coro_lite)

target_compile_features(coro_lite PUBLIC cxx_std_23)
target_include_directories(coro_lite PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
set_target_properties(coro_lite PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(CORO_LITE_VERBOSE_LOG "AUTO" CACHE STRING "Verbose logging: AUTO, ON or OFF")
set_property(CACHE CORO_LITE_VERBOSE_LOG PROPERTY STRINGS AUTO ON OFF)
set(CORO_LITE_LOG_TAG "CoroLite" CACHE STRING "Android log tag")
if (CORO_LITE_VERBOSE_LOG STREQUAL "AUTO")
    set(CORO_LITE_VERBOSE_VALUE "$<IF:$<CONFIG:Debug>,1,0>")
elseif (CORO_LITE_VERBOSE_LOG STREQUAL "ON")
    set(CORO_LITE_VERBOSE_VALUE 1)
elseif (CORO_LITE_VERBOSE_LOG STREQUAL "OFF")
    set(CORO_LITE_VERBOSE_VALUE 0)
else ()
    message(FATAL_ERROR "CORO_LITE_VERBOSE_LOG must be AUTO, ON or OFF")
endif ()
target_compile_definitions(coro_lite PUBLIC
        CORO_LITE_VERBOSE_LOG=${CORO_LITE_VERBOSE_VALUE}
        CORO_LITE_LOG_TAG="${CORO_LITE_LOG_TAG}"
)

find_package(Threads REQUIRED)
target_link_libraries(coro_lite PUBLIC Threads::Threads)
if (ANDROID)
    target_link_libraries(coro_lite PUBLIC log)
endif ()

target_compile_options(coro_lite PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        $<$<STREQUAL:${SANITIZER},address>:-fsanitize=address>
        $<$<STREQUAL:${SANITIZER},thread>:-fsanitize=thread>
        $<$<OR:$<STREQUAL:${SANITIZER},address>,$<STREQUAL:${SANITIZER},thread>>:-O1>
)
target_link_options(coro_lite PUBLIC
        $<$<STREQUAL:${SANITIZER},address>:-fsanitize=address>
        $<$<STREQUAL:${SANITIZER},thread>:-fsanitize=thread>
)

option(CORO_LITE_BUILD_TESTS "Build coro_lite tests" OFF)
if (CORO_LITE_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(coro_task_lifecycle_test tests/CoroTaskLifecycleTest.cpp)
    target_link_libraries(coro_task_lifecycle_test PRIVATE CoroLite::coro_lite)
    target_compile_options(coro_task_lifecycle_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME coro_task_lifecycle COMMAND coro_task_lifecycle_test)

    add_executable(socket_awaiter_test tests/SocketAwaiterTest.cpp)
    target_link_libraries(socket_awaiter_test PRIVATE CoroLite::coro_lite)
    target_compile_options(socket_awaiter_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME socket_awaiter COMMAND socket_awaiter_test)
    set_tests_properties(socket_awaiter PROPERTIES TIMEOUT 30)
    add_executable(runtime_regression_test tests/RuntimeRegressionTest.cpp)
    target_link_libraries(runtime_regression_test PRIVATE CoroLite::coro_lite)
    target_compile_options(runtime_regression_test PRIVATE -Wall -Wextra -Wpedantic -Werror
            $<$<STREQUAL:${SANITIZER},address>:-fsanitize=address>
            $<$<STREQUAL:${SANITIZER},thread>:-fsanitize=thread>)
    target_link_options(runtime_regression_test PRIVATE
            -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=timerfd_create
            -Wl,--wrap=timerfd_settime -Wl,--wrap=write -Wl,--wrap=epoll_ctl)
    foreach (mode scheduler socket timer)
        add_test(NAME coro_${mode}_regression COMMAND runtime_regression_test ${mode})
        set_tests_properties(coro_${mode}_regression PROPERTIES TIMEOUT 30)
    endforeach ()
    foreach (verbose 0 1)
        add_executable(logger_${verbose}_test tests/LoggerTest.cpp)
        target_compile_features(logger_${verbose}_test PRIVATE cxx_std_23)
        target_include_directories(logger_${verbose}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(logger_${verbose}_test PRIVATE CORO_LITE_VERBOSE_LOG=${verbose})
        target_compile_options(logger_${verbose}_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(logger_${verbose}_test PRIVATE Threads::Threads)
        add_test(NAME coro_logger_${verbose} COMMAND logger_${verbose}_test ${verbose})
    endforeach ()
    foreach (test_target coro_task_lifecycle_test socket_awaiter_test logger_0_test logger_1_test)
        target_compile_options(${test_target} PRIVATE
                $<$<STREQUAL:${SANITIZER},address>:-fsanitize=address>
                $<$<STREQUAL:${SANITIZER},thread>:-fsanitize=thread>)
        target_link_options(${test_target} PRIVATE
                $<$<STREQUAL:${SANITIZER},address>:-fsanitize=address>
                $<$<STREQUAL:${SANITIZER},thread>:-fsanitize=thread>)
    endforeach ()
endif ()
