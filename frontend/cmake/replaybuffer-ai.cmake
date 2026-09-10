if(OS_WINDOWS)
  target_sources(
    obs-studio
    PRIVATE
      replaybuffer/ReplayBufferAIEditor.cpp
      replaybuffer/ReplayBufferAIEditor.hpp
  )
endif()
