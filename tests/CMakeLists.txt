add_executable(keel_tests tests.cpp)
target_link_libraries(keel_tests PRIVATE keel::keel)
target_include_directories(keel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(keel_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(keel_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME keel_tests COMMAND keel_tests)
