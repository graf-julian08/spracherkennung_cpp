# SPDX-License-Identifier: MIT
#
# Simple PortAudio finder that works with system installs (Homebrew, apt, vcpkg).

find_path(PORTAUDIO_INCLUDE_DIR
  NAMES portaudio.h
  HINTS
    ENV PORTAUDIO_DIR
  PATH_SUFFIXES include
  PATHS
    /usr/include
    /usr/local/include
    /opt/homebrew/include
    /opt/local/include
)

find_library(PORTAUDIO_LIBRARY
  NAMES portaudio portaudio_static
  HINTS
    ENV PORTAUDIO_DIR
  PATH_SUFFIXES lib
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/homebrew/lib
    /opt/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PortAudio DEFAULT_MSG
  PORTAUDIO_LIBRARY PORTAUDIO_INCLUDE_DIR)

if(PortAudio_FOUND)
  set(PORTAUDIO_LIBRARIES ${PORTAUDIO_LIBRARY})
  set(PORTAUDIO_INCLUDE_DIRS ${PORTAUDIO_INCLUDE_DIR})
endif()

if(PortAudio_FOUND AND NOT TARGET PortAudio::PortAudio)
  add_library(PortAudio::PortAudio UNKNOWN IMPORTED)
  set_target_properties(PortAudio::PortAudio PROPERTIES
    IMPORTED_LOCATION "${PORTAUDIO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PORTAUDIO_INCLUDE_DIR}"
  )
endif()
