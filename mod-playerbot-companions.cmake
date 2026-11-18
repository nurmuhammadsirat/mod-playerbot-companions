if(MODULE_MOD-PLAYERBOT-COMPANIONS STREQUAL "disabled")
  return()
endif()

if(NOT MODULE_MOD-PLAYERBOT-COMPANIONS STREQUAL "static"
    OR NOT MODULE_MOD-PLAYERBOTS STREQUAL "static"
    OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mod-playerbots/src/Bot/PlayerbotMgr.h")
  message(FATAL_ERROR "mod-playerbot-companions requires mod-playerbots and both modules must be built statically.")
endif()
