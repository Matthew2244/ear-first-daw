cmake_minimum_required(VERSION 3.22)
project(EarFirstDAW VERSION 0.0.1)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE INTERNAL "")

add_subdirectory(external/tracktion_engine/modules/juce)
add_subdirectory(external/tracktion_engine/modules)

juce_add_console_app(capture-test NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(capture-test)
target_compile_features(capture-test PRIVATE cxx_std_20)
target_sources(capture-test PRIVATE src/capture_test.cpp)

target_compile_definitions(capture-test PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    TRACKTION_LOG_DEVICES=0)

target_link_libraries(capture-test PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)

if (APPLE)
    target_compile_options(capture-test PRIVATE "-fno-aligned-allocation")
endif()

# The disk-backed whole-session capture recorder and its self-test.
juce_add_console_app(capture-recorder-test NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(capture-recorder-test)
target_compile_features(capture-recorder-test PRIVATE cxx_std_20)
target_sources(capture-recorder-test PRIVATE src/capture_recorder_test.cpp src/capture/CaptureRecorder.cpp)
target_include_directories(capture-recorder-test PRIVATE src)
target_compile_definitions(capture-recorder-test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(capture-recorder-test PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_recommended_config_flags)

# capture-live: the disk recorder on a real audio interface.
juce_add_console_app(capture-live NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(capture-live)
target_compile_features(capture-live PRIVATE cxx_std_20)
target_sources(capture-live PRIVATE src/capture_live.cpp src/capture/CaptureRecorder.cpp)
target_include_directories(capture-live PRIVATE src)
target_compile_definitions(capture-live PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(capture-live PRIVATE
    juce::juce_core
    juce::juce_events
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_recommended_config_flags)

# capture-engine-test: kept takes land on the song timeline where they were played.
juce_add_console_app(capture-engine-test NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(capture-engine-test)
target_compile_features(capture-engine-test PRIVATE cxx_std_20)
target_sources(capture-engine-test PRIVATE src/capture_engine_test.cpp src/capture/EngineCapture.cpp src/capture/CaptureRecorder.cpp)
target_include_directories(capture-engine-test PRIVATE src)
target_compile_definitions(capture-engine-test PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    TRACKTION_LOG_DEVICES=0)
target_link_libraries(capture-engine-test PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)
if (APPLE)
    target_compile_options(capture-engine-test PRIVATE "-fno-aligned-allocation")
endif()

# commands-test: the command list and keymap.
juce_add_console_app(commands-test NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(commands-test)
target_compile_features(commands-test PRIVATE cxx_std_20)
target_sources(commands-test PRIVATE src/commands_test.cpp src/commands/Commands.cpp src/commands/Keymap.cpp)
target_include_directories(commands-test PRIVATE src)
target_compile_definitions(commands-test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(commands-test PRIVATE juce::juce_core juce::juce_recommended_config_flags)

# speech-test: the speaking layer's timing rules.
juce_add_console_app(speech-test NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(speech-test)
target_compile_features(speech-test PRIVATE cxx_std_20)
target_sources(speech-test PRIVATE src/speech_test.cpp src/speech/Speech.cpp)
target_include_directories(speech-test PRIVATE src)
target_compile_definitions(speech-test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(speech-test PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_recommended_config_flags)

# session-test: saving, autosave and crash recovery.
juce_add_console_app(session-test NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(session-test)
target_compile_features(session-test PRIVATE cxx_std_20)
target_sources(session-test PRIVATE src/session_test.cpp src/session/Session.cpp src/capture/CaptureRecorder.cpp)
target_include_directories(session-test PRIVATE src)
target_compile_definitions(session-test PRIVATE
    JUCE_PLUGINHOST_AU=1 JUCE_PLUGINHOST_VST3=1 JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 TRACKTION_LOG_DEVICES=0)
target_link_libraries(session-test PRIVATE
    tracktion::tracktion_core tracktion::tracktion_engine tracktion::tracktion_graph
    juce::juce_audio_devices juce::juce_audio_processors juce::juce_audio_utils
    juce::juce_recommended_config_flags)
if (APPLE)
    target_compile_options(session-test PRIVATE "-fno-aligned-allocation")
endif()

# The app, and its self-test (the same window code, never shown).
set(EARFIRST_APP_SOURCES
    src/app/Main.cpp src/app/AppCore.cpp src/app/Updater.cpp
    src/commands/Commands.cpp src/commands/Keymap.cpp src/speech/Speech.cpp
    src/session/Session.cpp src/capture/EngineCapture.cpp src/capture/CaptureRecorder.cpp)
if (APPLE)
    list(APPEND EARFIRST_APP_SOURCES src/app/Announce_mac.mm)
endif()
set(EARFIRST_APP_DEFS
    JUCE_PLUGINHOST_AU=1 JUCE_PLUGINHOST_VST3=1 JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 TRACKTION_LOG_DEVICES=0
    JUCE_APPLICATION_NAME_STRING="Ear First" JUCE_APPLICATION_VERSION_STRING="0.1.0")
set(EARFIRST_APP_LIBS
    tracktion::tracktion_core tracktion::tracktion_engine tracktion::tracktion_graph
    juce::juce_audio_devices juce::juce_audio_processors juce::juce_audio_utils juce::juce_gui_extra
    juce::juce_recommended_config_flags)

juce_add_gui_app(EarFirst
    PRODUCT_NAME "Ear First"
    VERSION 0.1.0
    BUNDLE_ID com.matthewwhitaker.earfirst
    COMPANY_NAME "Matthew Whitaker"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Ear First keeps everything you play on every input, so you can keep a take after you've played it."
    NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(EarFirst)
target_compile_features(EarFirst PRIVATE cxx_std_20)
target_sources(EarFirst PRIVATE ${EARFIRST_APP_SOURCES})
target_include_directories(EarFirst PRIVATE src)
target_compile_definitions(EarFirst PRIVATE ${EARFIRST_APP_DEFS})
target_link_libraries(EarFirst PRIVATE ${EARFIRST_APP_LIBS})

juce_add_console_app(ear-first-selftest NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(ear-first-selftest)
target_compile_features(ear-first-selftest PRIVATE cxx_std_20)
target_sources(ear-first-selftest PRIVATE ${EARFIRST_APP_SOURCES})
target_include_directories(ear-first-selftest PRIVATE src)
target_compile_definitions(ear-first-selftest PRIVATE ${EARFIRST_APP_DEFS} EARFIRST_SELFTEST_MAIN=1)
target_link_libraries(ear-first-selftest PRIVATE ${EARFIRST_APP_LIBS})

if (APPLE)
    target_compile_options(EarFirst PRIVATE "-fno-aligned-allocation")
    target_compile_options(ear-first-selftest PRIVATE "-fno-aligned-allocation")
endif()

# Sign the app after every build, so macOS keeps its permissions (microphone)
# across rebuilds. Set EARFIRST_SIGN_IDENTITY to your Developer ID, e.g.
#   cmake -B build -DEARFIRST_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
# Left as "-", builds are signed ad hoc and macOS asks again after each rebuild.
if (APPLE)
    set(EARFIRST_SIGN_IDENTITY "-" CACHE STRING "Code signing identity for the Ear First app")
    add_custom_command(TARGET EarFirst POST_BUILD
        COMMAND codesign --force --timestamp=none --options runtime
                --entitlements "${CMAKE_SOURCE_DIR}/src/app/EarFirst.entitlements"
                --sign "${EARFIRST_SIGN_IDENTITY}" "$<TARGET_BUNDLE_DIR:EarFirst>"
        COMMENT "Signing Ear First"
        VERBATIM)
endif()
