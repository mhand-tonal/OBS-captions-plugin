project(speech_api_deepgram_websocket)

message("SETTING UP SPEECH API DEEPGRAM WEBSOCKET")

find_package(CURL REQUIRED)

set(SPEECH_API_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/CaptionStream.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CaptionStream.h
        )

add_library(speech_api_deepgram_ws STATIC
        ${SPEECH_API_SOURCES}
        )

target_include_directories(speech_api_deepgram_ws PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(speech_api_deepgram_ws PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/json11)

target_link_libraries(speech_api_deepgram_ws CURL::libcurl)

if (WIN32)
    target_link_libraries(speech_api_deepgram_ws ws2_32)
endif ()

if (UNIX AND NOT APPLE)
    target_compile_options(speech_api_deepgram_ws PRIVATE "-fPIC")
endif ()
