cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(replaybuffer-ai MODULE)
add_library(OBS::replaybuffer-ai ALIAS replaybuffer-ai)

add_executable(replaybuffer-local-editor-worker worker/LocalEditorWorkerBootstrap.cpp)
set_target_properties(
  replaybuffer-local-editor-worker
  PROPERTIES
    FOLDER plugins/replaybuffer-ai
    OUTPUT_NAME "LocalEditorWorker"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit"
)

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

add_dependencies(replaybuffer-ai replaybuffer-local-editor-worker)
add_custom_command(
  TARGET replaybuffer-local-editor-worker
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:replaybuffer-local-editor-worker>/editor"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/editor"
          "$<TARGET_FILE_DIR:replaybuffer-local-editor-worker>/editor"
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
