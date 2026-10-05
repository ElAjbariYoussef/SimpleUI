# Parser-level tests. These cover the .sui sample from the project instructions,
# including its line comments and self-closing tags.

add_executable(parser_tests parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE sui)
add_test(NAME parser_tests COMMAND parser_tests)

# CSS parsing, cascade, and block/inline layout.
add_executable(layout_tests layout_tests.cpp)
target_link_libraries(layout_tests PRIVATE sui)
add_test(NAME layout_tests COMMAND layout_tests)

# Script registry, inline handlers, and the web-named DOM surface.
add_executable(script_tests script_tests.cpp)
target_link_libraries(script_tests PRIVATE sui)
add_test(NAME script_tests COMMAND script_tests)

# Prints what the engine measures for each documented capability. Not a pass/fail
# test: it is the evidence behind Difference.md.
add_executable(capability_probe capability_probe.cpp)
target_link_libraries(capability_probe PRIVATE sui)
