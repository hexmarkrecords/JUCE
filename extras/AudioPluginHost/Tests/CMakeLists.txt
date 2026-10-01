cmake_minimum_required(VERSION 3.22)
project(AudioPlayerTests VERSION 1.0 LANGUAGES C CXX)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(../../.. JUCE)
juce_add_console_app(AudioPlayerTests)
juce_generate_juce_header(AudioPlayerTests)
target_sources(AudioPlayerTests PRIVATE AudioPlayerTests.cpp)
target_compile_definitions(AudioPlayerTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    AUDIO_PLAYER_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/Assets")
target_link_libraries(AudioPlayerTests PRIVATE juce::juce_audio_utils)
enable_testing()
add_test(NAME AudioPlayerTests COMMAND AudioPlayerTests)
