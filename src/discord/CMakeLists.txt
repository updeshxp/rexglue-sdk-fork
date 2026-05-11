# rexdiscord - Discord Rich Presence client
# Windows and Linux only

if(NOT WIN32 AND NOT LINUX)
  return()
endif()

add_library(rexdiscord OBJECT discord_rpc.cpp)
add_library(rex::discord ALIAS rexdiscord)

target_include_directories(rexdiscord PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(rexdiscord PUBLIC rexcore)
