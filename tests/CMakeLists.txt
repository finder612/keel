add_executable(keel_tests tests.cpp)
target_link_libraries(keel_tests PRIVATE keel::keel)
target_include_directories(keel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The same suite again, built against the generated single header only, so a
# stale or broken amalgamation fails the build. It deliberately does not link
# keel::keel, which would put include/ on the path instead.
add_executable(keel_tests_single_header tests.cpp)
target_include_directories(keel_tests_single_header PRIVATE
    ${PROJECT_SOURCE_DIR}/single_include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(keel_tests_single_header PRIVATE cxx_std_17)
target_link_libraries(keel_tests_single_header PRIVATE Threads::Threads)

foreach(target keel_tests keel_tests_single_header)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endforeach()
