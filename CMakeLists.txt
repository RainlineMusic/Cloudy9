cmake_minimum_required(VERSION 3.22)
project(Cloudy9 VERSION 0.2.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED JUCE_DIR)
  set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../JUCE")
endif()
add_subdirectory(${JUCE_DIR} JUCE)
juce_add_plugin(Cloudy9
  COMPANY_NAME "Rainline Music"
  COMPANY_WEBSITE "https://github.com/RainlineMusic/Cloudy9"
  COMPANY_EMAIL "rainlinemusic@gmail.com"
  BUNDLE_ID "ru.rainlinemusic.cloudy9"
  AAX_IDENTIFIER "ru.rainlinemusic.cloudy9"
  PLUGIN_MANUFACTURER_CODE RnLn
  PLUGIN_CODE ClN9
  FORMATS VST3 AAX Standalone
  PRODUCT_NAME "Cloudy9"
  DESCRIPTION "Ninth-harmonic generator"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_CATEGORIES Fx Distortion
  AAX_CATEGORY Harmonic)
juce_generate_juce_header(Cloudy9)
target_sources(Cloudy9 PRIVATE
  Source/ChebyshevDSP.h
  Source/PluginProcessor.cpp Source/PluginProcessor.h
  Source/PluginEditor.cpp Source/PluginEditor.h)
target_compile_definitions(Cloudy9 PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(Cloudy9 PRIVATE juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
add_executable(Cloudy9DSPTest Tests/dsp_test.cpp)
enable_testing()
add_test(NAME Cloudy9DSP COMMAND Cloudy9DSPTest)
