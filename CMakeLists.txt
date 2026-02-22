cmake_minimum_required(VERSION 3.22)

project(MySampler VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Just add JUCE as a subdirectory - that's ALL you need!
# This single line gives you access to all JUCE CMake commands
add_subdirectory(lib/JUCE)

# Create your application
juce_add_gui_app(MySampler
    PRODUCT_NAME "My Sampler"
    COMPANY_NAME "YourCompany"
    VERSION ${PROJECT_VERSION}
)

# Add your source files
target_sources(MySampler
    PRIVATE
        src/Main.cpp
        src/MainComponent.cpp
        src/MainComponent.h
        src/UIComponents.cpp
        src/UIComponents.h
)

# Link required JUCE modules
target_link_libraries(MySampler
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_gui_basics
        juce::juce_gui_extra
)

# Add Windows-specific libraries HERE (after JUCE modules)
if (WIN32)
    target_link_libraries(MySampler PRIVATE vfw32.lib)
endif()

# Generate JUCE header
juce_generate_juce_header(MySampler)