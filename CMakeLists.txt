cmake_minimum_required(VERSION 3.25)

project(lume VERSION 0.1.0 LANGUAGES CXX)

option(LUME_BUILD_TESTS "Build Lume's tests" ON)
option(LUME_WITH_LOCAL_LLM "Enable local OpenAI-compatible LLM integration" ON)

add_library(lume_core
    src/assistant.cpp
    src/language.cpp
    src/ledger.cpp
    src/store.cpp
)
target_include_directories(lume_core PUBLIC include)
target_compile_features(lume_core PUBLIC cxx_std_26)
target_compile_options(lume_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

if(LUME_WITH_LOCAL_LLM)
    find_package(CURL QUIET)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
    if(CURL_FOUND AND NLOHMANN_JSON_INCLUDE_DIR)
        target_sources(lume_core PRIVATE src/openai_language.cpp)
        target_link_libraries(lume_core PRIVATE CURL::libcurl)
        target_include_directories(lume_core PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
        target_compile_definitions(lume_core PRIVATE LUME_HAS_LOCAL_LLM=1)
        message(STATUS "Lume local LLM integration: enabled")
    else()
        message(WARNING "libcurl or nlohmann/json.hpp not found; using deterministic language only")
    endif()
endif()

add_executable(lume src/main.cpp)
target_link_libraries(lume PRIVATE lume_core)

if(LUME_BUILD_TESTS)
    include(CTest)
    add_executable(lume_tests tests/lume_tests.cpp)
    target_link_libraries(lume_tests PRIVATE lume_core)
    add_test(NAME lume_tests COMMAND lume_tests)
endif()
