cmake_minimum_required(VERSION 3.22)
project(JBJAudio VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: use -DJUCE_DIR=<path to a JUCE checkout>, otherwise download it.
if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(JBJAudio
    PRODUCT_NAME "JBJAudio"
    COMPANY_NAME "JBJ"
    VERSION ${PROJECT_VERSION})

target_sources(JBJAudio PRIVATE
    Source/Main.cpp
    Source/MixerEngine.cpp
    Source/NoiseReducer.cpp
    Source/MainComponent.cpp)

target_compile_definitions(JBJAudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(JBJAudio PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
