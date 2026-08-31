add_executable(atlas_scheduler_regression
    atlas_scheduler_regression.cpp
)

target_link_libraries(atlas_scheduler_regression PRIVATE eoe_core)

add_test(NAME atlas_scheduler_regression COMMAND atlas_scheduler_regression)

add_executable(atlas_music_intent_regression
    atlas_music_intent_regression.cpp
)

target_link_libraries(atlas_music_intent_regression PRIVATE eoe_core)
add_dependencies(atlas_music_intent_regression faxiscripts)

add_test(NAME atlas_music_intent_regression COMMAND atlas_music_intent_regression)
add_test(NAME atlas_music_intent_cli_regression
    COMMAND atlas_music_intent_regression --cli $<TARGET_FILE:faxiscripts>
)

add_executable(atlas_music_event_regression
    atlas_music_event_regression.cpp
)

target_link_libraries(atlas_music_event_regression PRIVATE eoe_core)
add_dependencies(atlas_music_event_regression faxiscripts)
target_compile_definitions(atlas_music_event_regression PRIVATE
    EOE_CONFIG_SOURCE="${EOE_CONFIG_SOURCE}"
)

add_test(NAME atlas_music_event_regression COMMAND atlas_music_event_regression)
add_test(NAME atlas_music_event_cli_regression
    COMMAND atlas_music_event_regression --cli $<TARGET_FILE:faxiscripts>
)

add_executable(procedural_music_compiler
    procedural_music_compiler.cpp
)

target_link_libraries(procedural_music_compiler PRIVATE eoe_core)

add_test(NAME procedural_music_compiler COMMAND procedural_music_compiler)

add_test(NAME procedural_music_cli
    COMMAND ${CMAKE_COMMAND}
        -DEOE_CLI=$<TARGET_FILE:faxiscripts>
        -DFIXTURE_GENERATOR=$<TARGET_FILE:procedural_music_compiler>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/procedural_music_cli_test.cmake
)
