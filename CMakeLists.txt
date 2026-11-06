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
