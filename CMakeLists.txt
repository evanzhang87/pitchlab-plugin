cmake_minimum_required(VERSION 3.22)
project(PitchLab VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ------- JUCE (FetchContent，无需手动安装) -------
# JUCE 8.0.15: VST3 客户端已移除对 pluginterfaces/vst2.x 旧头(已绝版)的依赖
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ------- 插件格式：macOS 出 VST3+AU；Windows 出 VST3 -------
set(PITCHLAB_FORMATS VST3)
if(APPLE)
    list(APPEND PITCHLAB_FORMATS AU)
endif()

# VST3 需要 Steinberg VST3 SDK：CI 在 workflow 里显式克隆(--recursive，含
# pluginterfaces/vst2.x 旧头)并自检；本地构建请用 -DJUCE_VST3_SDK_DIR=<sdk路径>
if(NOT JUCE_VST3_SDK_DIR)
    message(WARNING "JUCE_VST3_SDK_DIR not set — VST3 目标将无法编译。"
                    "CI 中由 workflow 提供；本地请 -DJUCE_VST3_SDK_DIR=...")
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
