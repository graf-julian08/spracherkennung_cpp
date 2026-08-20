cmake_minimum_required(VERSION 3.20)

project(continuous_listening LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Whisper.cpp configuration
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_CLIENT OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)

include(FetchContent)

FetchContent_Declare(
  whispercpp
  GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
  GIT_TAG v1.5.1
)

FetchContent_MakeAvailable(whispercpp)

find_package(PortAudio REQUIRED)

# Find Mosquitto
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h
    HINTS /opt/homebrew/include /usr/local/include /usr/include)
find_library(MOSQUITTO_LIBRARY mosquitto
    HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)

if (MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
    message(STATUS "Found Mosquitto: ${MOSQUITTO_LIBRARY}")
    include_directories(${MOSQUITTO_INCLUDE_DIR})
else()
    message(FATAL_ERROR "Mosquitto library not found!")
endif()

add_executable(continuous_listening
  src/main.cpp
)

target_include_directories(continuous_listening PRIVATE ${whispercpp_SOURCE_DIR})

if(TARGET PortAudio::PortAudio)
  set(PORTAUDIO_LINK_TARGET PortAudio::PortAudio)
else()
  set(PORTAUDIO_LINK_TARGET ${PORTAUDIO_LIBRARIES})
  target_include_directories(continuous_listening PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
endif()

target_link_libraries(continuous_listening PRIVATE whisper ${PORTAUDIO_LINK_TARGET} ${CMAKE_DL_LIBS} ${MOSQUITTO_LIBRARY})

if (APPLE)
    target_link_libraries(continuous_listening PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreFoundation")
endif()

if(MSVC)
  target_compile_options(continuous_listening PRIVATE /W4 /permissive-)
else()
  target_compile_options(continuous_listening PRIVATE -Wall -Wextra -Wpedantic)
endif()
