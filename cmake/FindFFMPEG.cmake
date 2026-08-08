include(FindPackageHandleStandardArgs)

set(_ffmpeg_components ${FFMPEG_FIND_COMPONENTS})
if(NOT _ffmpeg_components)
  set(_ffmpeg_components avformat avcodec avutil)
endif()

find_package(PkgConfig QUIET)

if(FLVCONCAT_FFMPEG_STATIC)
  if(NOT FFMPEG_ROOT AND NOT DEFINED ENV{FFMPEG_ROOT})
    message(FATAL_ERROR "FLVCONCAT_FFMPEG_STATIC requires FFMPEG_ROOT")
  endif()

  foreach(_component IN LISTS _ffmpeg_components)
    string(TOUPPER "${_component}" _component_upper)
    find_path(FFMPEG_${_component_upper}_INCLUDE_DIR
      NAMES "lib${_component}/${_component}.h"
      HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT
      PATH_SUFFIXES include
      NO_DEFAULT_PATH)
    find_file(FFMPEG_${_component_upper}_LIBRARY
      NAMES "lib${_component}.a"
      HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT
      PATH_SUFFIXES lib lib64
      NO_DEFAULT_PATH)

    if(FFMPEG_${_component_upper}_INCLUDE_DIR AND FFMPEG_${_component_upper}_LIBRARY)
      add_library(FFMPEG::${_component} STATIC IMPORTED)
      set_target_properties(FFMPEG::${_component} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_${_component_upper}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${_component_upper}_INCLUDE_DIR}")
      if(UNIX)
        # The minimal Linux FFmpeg configuration has no external codecs or
        # protocols, but its static archives still use the system C runtime.
        set_property(TARGET FFMPEG::${_component} APPEND PROPERTY
          INTERFACE_LINK_LIBRARIES "m;pthread;dl")
      elseif(WIN32)
        set_property(TARGET FFMPEG::${_component} APPEND PROPERTY
          INTERFACE_LINK_LIBRARIES "ws2_32;secur32;bcrypt")
      endif()
      set(FFMPEG_${_component_upper}_FOUND TRUE)
      set(FFMPEG_${_component}_FOUND TRUE)
    endif()
  endforeach()
else()
  foreach(_component IN LISTS _ffmpeg_components)
    string(TOUPPER "${_component}" _component_upper)

    if(PkgConfig_FOUND)
      pkg_check_modules(PC_${_component_upper} QUIET IMPORTED_TARGET "lib${_component}")
    endif()

    if(TARGET PkgConfig::PC_${_component_upper})
      add_library(FFMPEG::${_component} ALIAS PkgConfig::PC_${_component_upper})
      set(FFMPEG_${_component_upper}_FOUND TRUE)
      set(FFMPEG_${_component}_FOUND TRUE)
    else()
      find_path(FFMPEG_${_component_upper}_INCLUDE_DIR
        NAMES "lib${_component}/${_component}.h"
        HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT
        PATH_SUFFIXES include)
      find_library(FFMPEG_${_component_upper}_LIBRARY
        NAMES ${_component} "lib${_component}.a"
        HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT
        PATH_SUFFIXES lib lib64)

      if(FFMPEG_${_component_upper}_INCLUDE_DIR AND FFMPEG_${_component_upper}_LIBRARY)
        add_library(FFMPEG::${_component} UNKNOWN IMPORTED)
        set_target_properties(FFMPEG::${_component} PROPERTIES
          IMPORTED_LOCATION "${FFMPEG_${_component_upper}_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${_component_upper}_INCLUDE_DIR}")
        if(WIN32)
          set_property(TARGET FFMPEG::${_component} APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES "ws2_32;secur32;bcrypt")
        endif()
        set(FFMPEG_${_component_upper}_FOUND TRUE)
        set(FFMPEG_${_component}_FOUND TRUE)
      endif()
    endif()
  endforeach()
endif()

set(_ffmpeg_required_vars)
foreach(_component IN LISTS _ffmpeg_components)
  string(TOUPPER "${_component}" _component_upper)
  list(APPEND _ffmpeg_required_vars FFMPEG_${_component_upper}_FOUND)
endforeach()

find_package_handle_standard_args(FFMPEG
  REQUIRED_VARS ${_ffmpeg_required_vars}
  HANDLE_COMPONENTS)
