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

# Optional read-only reuse of playerbots' bad-navmesh knowledge. Only enabled
# when the installed fork actually exposes the accessors we call, so an older
# or patched mod-playerbots still compiles.
set(MYBOTS_TRAVELMGR_HEADER "${MYBOTS_PLAYERBOTS_DIR}/src/Mgr/Travel/TravelMgr.h")
if(EXISTS "${MYBOTS_TRAVELMGR_HEADER}")
  file(READ "${MYBOTS_TRAVELMGR_HEADER}" MYBOTS_TRAVELMGR_SRC)
  string(FIND "${MYBOTS_TRAVELMGR_SRC}" "isBadMmap" MYBOTS_TM_HAS_BADMMAP)
  string(FIND "${MYBOTS_TRAVELMGR_SRC}" "define sTravelMgr" MYBOTS_TM_HAS_MACRO)
  if(NOT MYBOTS_TM_HAS_BADMMAP EQUAL -1 AND NOT MYBOTS_TM_HAS_MACRO EQUAL -1)
    add_definitions(-DMYBOTS_HAVE_TRAVELMGR)
    message(STATUS "mod-mybots: TravelMgr bad-grid reuse enabled")
  else()
    message(STATUS "mod-mybots: TravelMgr found but API mismatch, bad-grid reuse disabled")
  endif()
else()
  message(STATUS "mod-mybots: TravelMgr.h not found, bad-grid reuse disabled")
endif()
