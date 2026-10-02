# Parser-level tests. These cover the .sui sample from the project instructions,
# including its line comments and self-closing tags.

add_executable(parser_tests parser_tests.cpp)

target_link_libraries(parser_tests PRIVATE sui)

add_test(NAME parser_tests COMMAND parser_tests)