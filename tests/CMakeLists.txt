# Parser-level tests. These cover the .sui sample from the project instructions,
# including its line comments and self-closing tags.

add_executable(parser_tests parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE sui)
add_test(NAME parser_tests COMMAND parser_tests)

# CSS parsing, cascade, and block/inline layout.
add_executable(layout_tests layout_tests.cpp)
target_link_libraries(layout_tests PRIVATE sui)
add_test(NAME layout_tests COMMAND layout_tests)
