include(FindPackageHandleStandardArgs)

set(_ffmpeg_components ${FFMPEG_FIND_COMPONENTS})
if(NOT _ffmpeg_components)
  set(_ffmpeg_components avformat avcodec avutil)
endif()

find_package(PkgConfig QUIET)

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

set(_ffmpeg_required_vars)
foreach(_component IN LISTS _ffmpeg_components)
  string(TOUPPER "${_component}" _component_upper)
  list(APPEND _ffmpeg_required_vars FFMPEG_${_component_upper}_FOUND)
endforeach()

find_package_handle_standard_args(FFMPEG
  REQUIRED_VARS ${_ffmpeg_required_vars}
  HANDLE_COMPONENTS)
