cmake_minimum_required(VERSION 3.21)
project(CGPlayAIProtocolContracts LANGUAGES CXX)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Widgets Concurrent)
get_filename_component(CGPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(ai_protocol_contracts
    main.cpp
    "${CGPLAY_ROOT}/src/services/ai/OpenAIResponsesProvider.cpp"
    "${CGPLAY_ROOT}/src/services/ai/discovery/AIConnectionValidator.cpp"
    "${CGPLAY_ROOT}/src/services/ai/discovery/AIModelDiscovery.cpp"
    "${CGPLAY_ROOT}/src/services/ai/discovery/AIProviderDetector.cpp"
    "${CGPLAY_ROOT}/src/ui/app/AIConnectionWizardWidget.cpp")
target_compile_features(ai_protocol_contracts PRIVATE cxx_std_17)
target_include_directories(ai_protocol_contracts PRIVATE
    "${CGPLAY_ROOT}/src" "${CGPLAY_ROOT}/src/services"
    "${CGPLAY_ROOT}/src/features" "${CGPLAY_ROOT}/src/common")
target_link_libraries(ai_protocol_contracts PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Concurrent)
if(WIN32)
    target_compile_definitions(ai_protocol_contracts PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ai_protocol_contracts PRIVATE user32)
endif()
if(MSVC)
    target_compile_options(ai_protocol_contracts PRIVATE /utf-8 /W4)
endif()
