set(public_files
    "${CMAKE_CURRENT_SOURCE_DIR}/include/OpenLoco/Audio/AudioChannel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/OpenLoco/Audio/AudioEffect.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/OpenLoco/Audio/AudioEngine.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/OpenLoco/Audio/AudioFormat.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/OpenLoco/Audio/AudioHandle.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/OpenLoco/Audio/MidiRenderer.h"
)

set(private_files
    "${CMAKE_CURRENT_SOURCE_DIR}/src/AudioEngine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MidiRenderer.cpp"
)

set(public_link_libraries
    Core
    Diagnostics
)

set(private_link_libraries
    ${OPENAL_LIBRARIES}
    TinySoundFont
)

loco_add_library(Audio STATIC
    PUBLIC_FILES
        ${public_files}
    PRIVATE_FILES
        ${private_files}
    PUBLIC_LINK_LIBRARIES
        ${public_link_libraries}
    PRIVATE_LINK_LIBRARIES
        ${private_link_libraries}
)
