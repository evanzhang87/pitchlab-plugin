cmake_minimum_required(VERSION 3.22)
project(PitchLab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ------- JUCE (FetchContent，无需手动安装) -------
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        7.0.12
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ------- 插件格式：macOS 出 VST3+AU；Windows 出 VST3 -------
set(PITCHLAB_FORMATS VST3)
if(APPLE)
    list(APPEND PITCHLAB_FORMATS AU)
endif()

juce_add_plugin(PitchLab
    COMPANY_NAME              "PitchLab"
    PLUGIN_MANUFACTURER_CODE  PLAB
    PLUGIN_CODE               PLAB
    PRODUCT_NAME              "PitchLab"
    DESCRIPTION               "Bend/vibrato intonation visualizer"
    VERSION                   "1.0.0"
    FORMATS                   ${PITCHLAB_FORMATS}
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_NEEDS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE)

target_sources(PitchLab PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_include_directories(PitchLab PRIVATE Source)

# ------- 核心 DSP 单元测试(不依赖 JUCE，本地即可跑) -------
add_executable(dsp_test tests/dsp_test.cpp)
target_include_directories(dsp_test PRIVATE Source)
enable_testing()
add_test(NAME dsp_test COMMAND dsp_test)
