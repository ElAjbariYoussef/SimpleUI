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

# What the platform turns raw SDL mouse events into. Needs a video driver but no
# display, so the dummy one runs it without touching the window system.
add_executable(input_tests input_tests.cpp)
target_link_libraries(input_tests PRIVATE sui)
add_test(NAME input_tests COMMAND input_tests)
set_tests_properties(input_tests PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")

# Renderer geometry: radii clamping, outline sampling, and the gradient maths. None
# of it needs a device, so it runs in the ordinary test pass.
add_executable(render_tests render_tests.cpp)
target_link_libraries(render_tests PRIVATE sui)
# The font metrics test needs the bundled TTF, which lives outside the build tree.
target_compile_definitions(render_tests PRIVATE SUI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME render_tests COMMAND render_tests)

# Paint order and stacking contexts: a pure walk over a laid-out document, so it
# needs no device either.
add_executable(paint_tests paint_tests.cpp)
target_link_libraries(paint_tests PRIVATE sui)
add_test(NAME paint_tests COMMAND paint_tests)

# Prints what the engine measures for each documented capability. Not a pass/fail
# test: it is the evidence behind Difference.md.
add_executable(capability_probe capability_probe.cpp)
target_link_libraries(capability_probe PRIVATE sui)
