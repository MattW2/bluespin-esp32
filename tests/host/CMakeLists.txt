cmake_minimum_required(VERSION 3.16)
project(bluespin_esp32_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(test_audio_core
  test_audio_core.cpp
)

target_include_directories(test_audio_core PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../components/bt_a2dp_sink"
)

if(MSVC)
  target_compile_options(test_audio_core PRIVATE /W4)
else()
  target_compile_options(test_audio_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME audio_core COMMAND test_audio_core)
