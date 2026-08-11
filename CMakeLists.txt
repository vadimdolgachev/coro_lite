cmake_minimum_required(VERSION 3.22.1)
project(coro_lite LANGUAGES CXX)

add_library(coro_lite STATIC
        CancellationToken.cpp
        CoroTask.cpp
        Endpoint.cpp
        Socket.cpp
)
add_library(CoroLite::coro_lite ALIAS coro_lite)

target_compile_features(coro_lite PUBLIC cxx_std_23)
target_include_directories(coro_lite PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
set_target_properties(coro_lite PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
endif ()
