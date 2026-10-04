cmake_minimum_required(VERSION 3.22)

project(JaibaSampler VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Just add JUCE as a subdirectory - that's ALL you need!
# This single line gives you access to all JUCE CMake commands
add_subdirectory(lib/JUCE)

# Create your application
juce_add_gui_app(JaibaSampler
    PRODUCT_NAME "JaibaSampler"
    COMPANY_NAME "Jaiba"
    VERSION ${PROJECT_VERSION}
)

# Add your source files
target_sources(JaibaSampler
    PRIVATE
        src/Main.cpp
        src/MainComponent.cpp
        src/MainComponent.h
        src/SampleCard.h
        src/UIComponents.cpp
        src/UIComponents.h
        src/ConfigurationManager.h
        src/MidiActivityLight.h
        src/Sequencer.h
        src/SequencerGrid.h
)

# Link required JUCE modules
target_link_libraries(JaibaSampler
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
)

# This app uses no WebBrowserComponent and no JUCE network (curl) APIs.
# JUCE enables JUCE_WEB_BROWSER / JUCE_USE_CURL by default on Linux, which would
# pull in webkit2gtk/GTK headers and libcurl; disabling them keeps the Linux
# build lean and portable. Harmless on other platforms (flags unused there).
target_compile_definitions(JaibaSampler
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

# Add Windows-specific libraries HERE (after JUCE modules)
if (WIN32)
    target_link_libraries(JaibaSampler PRIVATE vfw32.lib)
endif()

# Generate JUCE header
juce_generate_juce_header(JaibaSampler)