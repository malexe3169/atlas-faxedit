if(NOT DEFINED EOE_CLI)
    message(FATAL_ERROR "EOE_CLI was not provided")
endif()
if(NOT DEFINED FIXTURE_GENERATOR)
    message(FATAL_ERROR "FIXTURE_GENERATOR was not provided")
endif()

set(TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/procedural-music-cli")
file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")
set(SOURCE "${TEST_DIR}/worldscore-minimal.mml")
execute_process(
    COMMAND "${FIXTURE_GENERATOR}" --write-fixture "${SOURCE}"
    RESULT_VARIABLE FIXTURE_RESULT
    OUTPUT_VARIABLE FIXTURE_STDOUT
    ERROR_VARIABLE FIXTURE_STDERR
)
if(NOT FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "fixture generation failed: ${FIXTURE_STDOUT}${FIXTURE_STDERR}")
endif()

execute_process(
    COMMAND "${EOE_CLI}" pmusic-compile "${SOURCE}" "${TEST_DIR}/long.json"
    RESULT_VARIABLE LONG_RESULT
    OUTPUT_VARIABLE LONG_STDOUT
    ERROR_VARIABLE LONG_STDERR
)
if(NOT LONG_RESULT EQUAL 0)
    message(FATAL_ERROR "pmusic-compile failed: ${LONG_STDOUT}${LONG_STDERR}")
endif()

execute_process(
    COMMAND "${EOE_CLI}" pmc "${SOURCE}" "${TEST_DIR}/short.json"
    RESULT_VARIABLE SHORT_RESULT
    OUTPUT_VARIABLE SHORT_STDOUT
    ERROR_VARIABLE SHORT_STDERR
)
if(NOT SHORT_RESULT EQUAL 0)
    message(FATAL_ERROR "pmc failed: ${SHORT_STDOUT}${SHORT_STDERR}")
endif()

file(READ "${TEST_DIR}/long.json" LONG_JSON)
file(READ "${TEST_DIR}/short.json" SHORT_JSON)
if(NOT LONG_JSON STREQUAL SHORT_JSON)
    message(FATAL_ERROR "long and short commands emitted different JSON")
endif()
if(NOT LONG_JSON MATCHES "faxanadu-procedural-music-kit-v2"
        OR NOT LONG_JSON MATCHES "\"index\": 16"
        OR NOT LONG_JSON MATCHES "\"index\": 113"
        OR NOT LONG_JSON MATCHES "\"event_index\": 2"
        OR NOT LONG_JSON MATCHES "\"phrase_count\": 98"
        OR LONG_JSON MATCHES "\"kind\": \"stock\"")
    message(FATAL_ERROR "authored-only procedural-music JSON changed")
endif()

# Metadata comments remain valid input to the ordinary artist renderers.
execute_process(
    COMMAND "${EOE_CLI}" m2m "${SOURCE}" "${TEST_DIR}/midi"
    RESULT_VARIABLE MIDI_RESULT
    OUTPUT_VARIABLE MIDI_STDOUT
    ERROR_VARIABLE MIDI_STDERR
)
if(NOT MIDI_RESULT EQUAL 0 OR NOT EXISTS "${TEST_DIR}/midi-01.mid")
    message(FATAL_ERROR "m2m rejected annotated MML: ${MIDI_STDOUT}${MIDI_STDERR}")
endif()

# Unsupported compatibility metadata must fail atomically.
file(READ "${SOURCE}" SOURCE_TEXT)
string(REPLACE "quantum=1" "quantum=1 stock_playback=retain"
    INVALID_SOURCE_TEXT "${SOURCE_TEXT}")
file(WRITE "${TEST_DIR}/invalid.mml" "${INVALID_SOURCE_TEXT}")
file(WRITE "${TEST_DIR}/atomic.json" "sentinel\n")
execute_process(
    COMMAND "${EOE_CLI}" pmc "${TEST_DIR}/invalid.mml" "${TEST_DIR}/atomic.json"
    RESULT_VARIABLE INVALID_RESULT
    OUTPUT_VARIABLE INVALID_STDOUT
    ERROR_VARIABLE INVALID_STDERR
)
if(INVALID_RESULT EQUAL 0)
    message(FATAL_ERROR "pmc accepted removed stock_playback compatibility metadata")
endif()
file(READ "${TEST_DIR}/atomic.json" ATOMIC_OUTPUT)
if(NOT ATOMIC_OUTPUT STREQUAL "sentinel\n")
    message(FATAL_ERROR "failed compilation changed the existing output file")
endif()
