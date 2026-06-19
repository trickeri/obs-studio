cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Core Network)

add_library(nul-voice-control MODULE)
add_library(OBS::nul-voice-control ALIAS nul-voice-control)

target_sources(
  nul-voice-control
  PRIVATE
    nul-voice-control.cpp
    nul-voicechat-listener.cpp
    nul-voicechat-listener.hpp
)

target_link_libraries(
  nul-voice-control
  PRIVATE
    OBS::frontend-api
    OBS::libobs
    Qt::Core
    Qt::Network
)

set_target_properties_obs(
  nul-voice-control
  PROPERTIES FOLDER frontend
             PREFIX ""
             AUTOMOC ON
)
