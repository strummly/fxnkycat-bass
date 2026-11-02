cmake_minimum_required(VERSION 3.22)

# Mac: one universal build for Apple Silicon and Intel, running on macOS 10.13 or newer.
# (Must be set before project(). Ignored on Windows.)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Mac architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

project(BassChain VERSION 1.0.0)

# Windows: build the C++ runtime into the plugin so it loads on PCs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is downloaded automatically the first time you configure.
include(FetchContent)
FetchContent_Declare(
    JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.4.zip
)
FetchContent_MakeAvailable(JUCE)

# VST3 everywhere; Audio Unit (for Logic / GarageBand) on Mac.
if(APPLE)
    set(BASS_FORMATS VST3 AU)
else()
    set(BASS_FORMATS VST3)
endif()

juce_add_plugin(BassChain
    COMPANY_NAME "catfxnk creative"
    BUNDLE_ID com.fxnkycat.bass
    PLUGIN_MANUFACTURER_CODE Fxnk
    PLUGIN_CODE FxBs
    FORMATS ${BASS_FORMATS}
    PRODUCT_NAME "fxnkycat bass"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Dynamics EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD FALSE)

# Logo image, compiled into the plugin
juce_add_binary_data(BassChainAssets SOURCES Resources/logo.png)

target_sources(BassChain PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PluginInfo.cpp)

target_compile_definitions(BassChain PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1      # host one external VST3 as an insert
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0)

target_link_libraries(BassChain
    PRIVATE
        BassChainAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
