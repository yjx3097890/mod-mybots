# mod-mybots — depends on official mod-playerbots headers.
# Sources under src/ are picked up by AzerothCore module discovery.
# This file only injects playerbots include paths onto worldserver.

function(mybots_locate_playerbots out_var)
  set(_candidates
    "${CMAKE_SOURCE_DIR}/modules/mod-playerbots"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mod-playerbots"
  )
  foreach(_dir ${_candidates})
    if(EXISTS "${_dir}/src/Script/Playerbots.h")
      set(${out_var} "${_dir}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

mybots_locate_playerbots(MYBOTS_PLAYERBOTS_DIR)

if(NOT MYBOTS_PLAYERBOTS_DIR)
  message(FATAL_ERROR
    "mod-mybots requires official mod-playerbots. "
    "Clone it to azerothcore-wotlk/modules/mod-playerbots")
endif()

set(MYBOTS_PB_INCLUDES
  "${MYBOTS_PLAYERBOTS_DIR}/src"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Script"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Bot"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Bot/Cmd"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Bot/Engine"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Bot/Factory"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Ai"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Ai/Base"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Ai/Base/Actions"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Ai/Base/Strategy"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Ai/Base/Trigger"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Ai/Base/Value"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Ai/Base/Util"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Mgr"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Mgr/Travel"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Util"
  "${MYBOTS_PLAYERBOTS_DIR}/src/Db"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  CACHE INTERNAL "mod-mybots + playerbots include dirs"
)

include_directories(${MYBOTS_PB_INCLUDES})

function(mybots_attach_includes)
  if(TARGET worldserver)
    target_include_directories(worldserver PRIVATE ${MYBOTS_PB_INCLUDES})
  endif()
endfunction()

if(TARGET worldserver)
  mybots_attach_includes()
else()
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL mybots_attach_includes)
endif()

message(STATUS "mod-mybots: using playerbots at ${MYBOTS_PLAYERBOTS_DIR}")
