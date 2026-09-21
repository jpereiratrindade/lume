cmake_minimum_required(VERSION 3.25)

project(lume VERSION 0.1.0 LANGUAGES CXX)

option(LUME_BUILD_TESTS "Build Lume's tests" ON)

add_library(lume_core
    src/assistant.cpp
    src/language.cpp
    src/store.cpp
)
target_include_directories(lume_core PUBLIC include)
target_compile_features(lume_core PUBLIC cxx_std_26)
target_compile_options(lume_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(lume src/main.cpp)
target_link_libraries(lume PRIVATE lume_core)

if(LUME_BUILD_TESTS)
    include(CTest)
    add_executable(lume_tests tests/lume_tests.cpp)
    target_link_libraries(lume_tests PRIVATE lume_core)
    add_test(NAME lume_tests COMMAND lume_tests)
endif()
