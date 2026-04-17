project(caption_stream)

message("BUILD_SHARED_LIBS: ${BUILD_SHARED_LIBS}")

include_directories("thirdparty")
include_directories(".")

# Always build both speech API backends for runtime selection
add_subdirectory(speech_apis/google_http_older)
add_subdirectory(speech_apis/deepgram_websocket)

set(caption_stream_SOURCES
        utils.h
        ContinuousCaptions.cpp
        thirdparty/json11/json11.cpp
        )

set(caption_stream_HEADERS
        thirdparty/cameron314/concurrentqueue.h
        thirdparty/cameron314/blockingconcurrentqueue.h
        utils.h
        CaptionResult.h
        CaptionStream.h
        ContinuousCaptions.h
        )

add_library(caption_stream STATIC
        ${caption_stream_SOURCES}
        ${caption_stream_HEADERS}
        )

target_include_directories(caption_stream PUBLIC ./)
target_include_directories(caption_stream PUBLIC thirdparty/)
target_link_libraries(caption_stream speech_api_google_http speech_api_deepgram_ws)

if (WIN32 AND NOT BUILD_SHARED_LIBS)
    message(BUILD_SHARED_LIBS: setting win flags)
    foreach (flag_var
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)

        if (${flag_var} MATCHES "/MD")
            message("before: ${flag_var}: " ${${flag_var}}"")
            string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
            message("after : ${flag_var}: " ${${flag_var}}"")
        endif ()
    endforeach ()
endif ()

if (UNIX AND NOT APPLE)
    target_compile_options(caption_stream PRIVATE "-fPIC")
endif ()
