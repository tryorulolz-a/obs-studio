cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(replaybuffer-ai MODULE)
add_library(OBS::replaybuffer-ai ALIAS replaybuffer-ai)

target_sources(
  replaybuffer-ai
  PRIVATE
    replaybuffer-ai.cpp
    ../../frontend/replaybuffer/ReplayBufferAIEditor.cpp
    ../../frontend/replaybuffer/ReplayBufferAIEditor.hpp
)

target_link_libraries(
  replaybuffer-ai
  PRIVATE
    OBS::frontend-api
    OBS::libobs
    Qt::Widgets
)

set_target_properties_obs(
  replaybuffer-ai
  PROPERTIES
    FOLDER plugins/replaybuffer-ai
    PREFIX ""
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)
