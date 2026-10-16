cmake_minimum_required(VERSION 3.24)
project(VideoPlayerHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VP_ENGINE "playbackcore" CACHE STRING "Video player backend: playbackcore or mpv")
set_property(CACHE VP_ENGINE PROPERTY STRINGS playbackcore mpv)
set(PLAYBACKCORE_DIR "" CACHE PATH "PlaybackCore source folder (include/ and build-ffmpeg/)")
set(PLAYBACKCORE_BUILD "${PLAYBACKCORE_DIR}/build-ffmpeg" CACHE PATH "PlaybackCore build folder with playbackcore.lib/.dll")
set(FRAME_INTERPOLATION_DLL "${PLAYBACKCORE_DIR}/plugins/FrameInterpolation/build-ninja/PlaybackCore.FrameInterpolation.dll" CACHE FILEPATH "AMF frame interpolation plugin DLL")
set(LIBMPV_DIR "" CACHE PATH "Pinned libmpv runtime and include folder")
set(FFMPEG_DIR "" CACHE PATH "FFmpeg LGPL shared development package root")
set(WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2" CACHE PATH "Extracted Microsoft.Web.WebView2 NuGet package")
option(VP_BUILD_TEST_HARNESS "Build a separate MPV acceptance harness with debug-only hooks" OFF)

if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
    message(FATAL_ERROR "WebView2 SDK not found in ${WEBVIEW2_DIR} (build.ps1 downloads it)")
endif()

add_executable(VideoPlayer WIN32 src/main.cpp src/lan_server.cpp src/app.manifest src/app.rc)
# Release version (repo-root VERSION.txt, e.g. 0.1.0-beta) for the exe's file properties; 0.0.0 without it. The numeric
# FILEVERSION takes only the leading x.y.z (a -beta suffix stays in the version strings).
set(VP_VERSION "0.0.0")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt" VP_VERSION LIMIT_COUNT 1)
    string(STRIP "${VP_VERSION}" VP_VERSION)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" VP_VERSION_NUMERIC "${VP_VERSION}")
if(NOT VP_VERSION_NUMERIC)
    message(FATAL_ERROR "VERSION.txt must start with x.y.z, got ${VP_VERSION}")
endif()
string(REPLACE "." "," VP_VERSION_COMMAS "${VP_VERSION_NUMERIC}")
set_source_files_properties(src/app.rc PROPERTIES COMPILE_DEFINITIONS "VP_VERSION_STR=\"${VP_VERSION}\";VP_VERSION_NUM=${VP_VERSION_COMMAS},0")
target_compile_definitions(VideoPlayer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(VideoPlayer PRIVATE /utf-8 /W3 /permissive-)
endif()
target_include_directories(VideoPlayer PRIVATE src "${WEBVIEW2_DIR}/build/native/include")
target_link_libraries(VideoPlayer PRIVATE
    "${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    dcomp d3d11 dxgi ole32 oleaut32 shell32 shlwapi winhttp user32 gdi32 advapi32 version ws2_32)

if(VP_ENGINE STREQUAL "mpv")
    if(NOT EXISTS "${LIBMPV_DIR}/include/mpv/client.h" OR NOT EXISTS "${LIBMPV_DIR}/libmpv-2.dll")
        message(FATAL_ERROR "LIBMPV_DIR must contain include/mpv/client.h and the pinned libmpv-2.dll")
    endif()
    if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h" OR NOT EXISTS "${FFMPEG_DIR}/lib/avcodec.lib")
        message(FATAL_ERROR "FFMPEG_DIR must point to the BtbN win64 LGPL shared development package")
    endif()
    target_sources(VideoPlayer PRIVATE src/mpv_backend.cpp src/media_tools.cpp src/enhancement_compiler.cpp src/mpv_config_safety.cpp src/remux_stream.cpp
        src/gpu_adapter_identity.cpp)
    target_compile_definitions(VideoPlayer PRIVATE VP_ENGINE_MPV=1 _WIN32_WINNT=0x0A00)
    if(MSVC)
        # Keep symbols in the MPV host so crash-tripwire dumps can be resolved.
        target_compile_options(VideoPlayer PRIVATE /Zi)
        target_link_options(VideoPlayer PRIVATE /DEBUG /INCREMENTAL:NO)
        set_target_properties(VideoPlayer PROPERTIES
            PDB_NAME VideoPlayer
            PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            COMPILE_PDB_NAME VideoPlayer-compile
            COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    target_include_directories(VideoPlayer PRIVATE "${LIBMPV_DIR}/include" "${FFMPEG_DIR}/include")
    target_link_libraries(VideoPlayer PRIVATE
        dbghelp
        "${FFMPEG_DIR}/lib/avcodec.lib"
        "${FFMPEG_DIR}/lib/avformat.lib"
        "${FFMPEG_DIR}/lib/avutil.lib"
        "${FFMPEG_DIR}/lib/swresample.lib"
        "${FFMPEG_DIR}/lib/swscale.lib")

    add_custom_command(TARGET VideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBMPV_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:VideoPlayer>/libmpv-2.dll"
        VERBATIM)
    file(GLOB VP_FFMPEG_RUNTIME "${FFMPEG_DIR}/bin/*.dll")
    if(NOT VP_FFMPEG_RUNTIME)
        message(FATAL_ERROR "FFMPEG_DIR/bin has no runtime DLLs")
    endif()
    add_custom_command(TARGET VideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VP_FFMPEG_RUNTIME} "$<TARGET_FILE_DIR:VideoPlayer>"
        VERBATIM)

    if(VP_BUILD_TEST_HARNESS AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mpv_acceptance_harness.cpp")
        message(FATAL_ERROR "VP_BUILD_TEST_HARNESS needs tools/mpv_acceptance_harness.cpp (not part of the public source)")
    endif()
    if(VP_BUILD_TEST_HARNESS)
        # This executable is separate from VideoPlayer.exe. Debug-only test hooks
        # are never compiled into the distributable Release player.
        add_executable(MpvAcceptanceHarness
            ../tools/mpv_acceptance_harness.cpp
            src/mpv_backend.cpp
            src/enhancement_compiler.cpp
            src/mpv_config_safety.cpp
            src/gpu_adapter_identity.cpp
            src/lan_server.cpp)
        target_compile_definitions(MpvAcceptanceHarness PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0A00 VP_ENGINE_MPV=1 VP_TEST_HARNESS=1)
        target_include_directories(MpvAcceptanceHarness PRIVATE src "${LIBMPV_DIR}/include" "${FFMPEG_DIR}/include")
        if(MSVC)
            target_compile_options(MpvAcceptanceHarness PRIVATE /utf-8 /W3 /permissive- /EHsc /Zi)
            target_link_options(MpvAcceptanceHarness PRIVATE /DEBUG /INCREMENTAL:NO)
            set_target_properties(MpvAcceptanceHarness PROPERTIES
                PDB_NAME MpvAcceptanceHarness
                PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
        target_link_libraries(MpvAcceptanceHarness PRIVATE
            dbghelp psapi winhttp ws2_32 dcomp d3d11 dxgi ole32 oleaut32 shell32 shlwapi winhttp user32 gdi32 advapi32 version
            "${FFMPEG_DIR}/lib/avcodec.lib"
            "${FFMPEG_DIR}/lib/avformat.lib"
            "${FFMPEG_DIR}/lib/avutil.lib"
            "${FFMPEG_DIR}/lib/swresample.lib"
            "${FFMPEG_DIR}/lib/swscale.lib")
        add_custom_command(TARGET MpvAcceptanceHarness POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBMPV_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:MpvAcceptanceHarness>/libmpv-2.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VP_FFMPEG_RUNTIME} "$<TARGET_FILE_DIR:MpvAcceptanceHarness>"
            VERBATIM)
    endif()
elseif(VP_ENGINE STREQUAL "playbackcore")
    if(NOT EXISTS "${PLAYBACKCORE_DIR}/include/playbackcore_api.h")
        message(FATAL_ERROR "PLAYBACKCORE_DIR does not contain include/playbackcore_api.h: ${PLAYBACKCORE_DIR}")
    endif()
    target_include_directories(VideoPlayer PRIVATE "${PLAYBACKCORE_DIR}/include")

    # LAN browser remux uses FFmpeg's libraries from the PlaybackCore build cache.
    load_cache("${PLAYBACKCORE_BUILD}" READ_WITH_PREFIX PC_ AVCODEC_INCLUDE_DIR AVCODEC_LIBRARY AVFORMAT_LIBRARY AVUTIL_LIBRARY SWRESAMPLE_LIBRARY SWSCALE_LIBRARY)
    if(PC_AVCODEC_INCLUDE_DIR AND PC_AVCODEC_LIBRARY AND PC_AVFORMAT_LIBRARY AND PC_AVUTIL_LIBRARY AND PC_SWRESAMPLE_LIBRARY AND PC_SWSCALE_LIBRARY)
        target_sources(VideoPlayer PRIVATE src/remux_stream.cpp)
        target_include_directories(VideoPlayer PRIVATE "${PC_AVCODEC_INCLUDE_DIR}")
        target_link_libraries(VideoPlayer PRIVATE "${PC_AVCODEC_LIBRARY}" "${PC_AVFORMAT_LIBRARY}" "${PC_AVUTIL_LIBRARY}" "${PC_SWRESAMPLE_LIBRARY}" "${PC_SWSCALE_LIBRARY}")
    else()
        message(WARNING "FFmpeg not found in the PlaybackCore build cache: LAN browser streams are disabled")
        target_compile_definitions(VideoPlayer PRIVATE VP_NO_REMUX=1)
    endif()

    # The official build remains the default engine and keeps its existing import/runtime layout.
    set(VP_PLAYBACKCORE_IMPORT "${PLAYBACKCORE_BUILD}/playbackcore.import.lib")
    if(NOT EXISTS "${VP_PLAYBACKCORE_IMPORT}")
        set(VP_PLAYBACKCORE_IMPORT "${PLAYBACKCORE_BUILD}/playbackcore.lib")
    endif()
    target_link_libraries(VideoPlayer PRIVATE "${VP_PLAYBACKCORE_IMPORT}")
    file(GLOB VP_CORE_RUNTIME "${PLAYBACKCORE_BUILD}/*.dll" "${PLAYBACKCORE_BUILD}/playbackcore.pdb")
    add_custom_command(TARGET VideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VP_CORE_RUNTIME} "$<TARGET_FILE_DIR:VideoPlayer>"
        VERBATIM)

    set(VP_RIFE_MODEL "${PLAYBACKCORE_BUILD}/models/rife_v4.6.onnx")
    if(EXISTS "${PLAYBACKCORE_BUILD}/PlaybackCore.RIFE.dll" AND EXISTS "${VP_RIFE_MODEL}")
        add_custom_command(TARGET VideoPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VideoPlayer>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VP_RIFE_MODEL}" "$<TARGET_FILE_DIR:VideoPlayer>/models/rife_v4.6.onnx"
            VERBATIM)
    endif()
    set(VP_RIFE_LITE_MODEL "${PLAYBACKCORE_BUILD}/models/rife_v4.25_lite.onnx")
    if(EXISTS "${PLAYBACKCORE_BUILD}/PlaybackCore.RIFE.dll" AND EXISTS "${VP_RIFE_LITE_MODEL}")
        add_custom_command(TARGET VideoPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VideoPlayer>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VP_RIFE_LITE_MODEL}" "$<TARGET_FILE_DIR:VideoPlayer>/models/rife_v4.25_lite.onnx"
            VERBATIM)
    endif()
    foreach(vp_rife_extra rife_v4.6_fp16.onnx rife_v4.6_fp16_flow.onnx)
        if(EXISTS "${PLAYBACKCORE_BUILD}/PlaybackCore.RIFE.dll" AND EXISTS "${PLAYBACKCORE_BUILD}/models/${vp_rife_extra}")
            add_custom_command(TARGET VideoPlayer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VideoPlayer>/models"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PLAYBACKCORE_BUILD}/models/${vp_rife_extra}" "$<TARGET_FILE_DIR:VideoPlayer>/models/${vp_rife_extra}"
                VERBATIM)
        endif()
    endforeach()
    set(VP_RIFE_RESTORE_MODEL "${PLAYBACKCORE_BUILD}/models/Zarxrax_Anime1080Fixer_SUC_x1_fp16_op17.onnx")
    if(EXISTS "${VP_RIFE_RESTORE_MODEL}")
        add_custom_command(TARGET VideoPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VideoPlayer>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VP_RIFE_RESTORE_MODEL}" "$<TARGET_FILE_DIR:VideoPlayer>/models/Zarxrax_Anime1080Fixer_SUC_x1_fp16_op17.onnx"
            VERBATIM)
    endif()
    foreach(vp_ajn_model animejanai_v2_l1_fp16_op17.onnx animejanai_v2_l1_fp16.onnx animejanai_v2_ultracompact_fp16.onnx animejanai_hd_v3_superultracompact_fp16.onnx animejanai_hd_v3_ultracompact_fp16.onnx)
        if(EXISTS "${PLAYBACKCORE_BUILD}/models/${vp_ajn_model}")
            add_custom_command(TARGET VideoPlayer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VideoPlayer>/models"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PLAYBACKCORE_BUILD}/models/${vp_ajn_model}" "$<TARGET_FILE_DIR:VideoPlayer>/models/${vp_ajn_model}"
                VERBATIM)
        endif()
    endforeach()
    if(EXISTS "${FRAME_INTERPOLATION_DLL}")
        add_custom_command(TARGET VideoPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FRAME_INTERPOLATION_DLL}" "$<TARGET_FILE_DIR:VideoPlayer>"
            VERBATIM)
    else()
        message(WARNING "Frame interpolation plugin not found: ${FRAME_INTERPOLATION_DLL}")
    endif()
else()
    message(FATAL_ERROR "VP_ENGINE must be playbackcore or mpv, got '${VP_ENGINE}'")
endif()

# Header dependencies must not rely on cl /showIncludes alone. Ninja learns headers from that output by a localized
# prefix (zh-TW: "注意: 包含檔案:") that CMake detects once per build directory in the console code page of that first
# configure; the main checkout's host/build-mpv recorded it mangled ("?" bytes in rules.ninja msvc_deps_prefix), so ninja
# tracked no headers there. When f214b97 grew MpvBackend in mpv_backend.h, only mpv_backend.cpp was rebuilt: the stale
# main.cpp still allocated the old sizeof(MpvBackend) (0xEF8, now 0xF58) and the constructor wrote past the heap block,
# an access violation in the next allocation on every start (report 24). Every source therefore also depends on all of
# the host's own headers; a header edit rebuilds the whole (small) host whatever the code page.
file(GLOB VP_HOST_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(vp_target VideoPlayer MpvAcceptanceHarness)
    if(TARGET ${vp_target})
        get_target_property(vp_target_sources ${vp_target} SOURCES)
        foreach(vp_source IN LISTS vp_target_sources)
            if(vp_source MATCHES "[.]cpp$")
                set_source_files_properties(${vp_source} PROPERTIES OBJECT_DEPENDS "${VP_HOST_HEADERS}")
            endif()
        endforeach()
    endif()
endforeach()
if(CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "[?]")
    message(WARNING "This build directory recorded a mangled cl /showIncludes prefix ('${CMAKE_CL_SHOWINCLUDES_PREFIX}'): "
        "ninja tracks no header dependencies here beyond host/src/*.h. Delete ${CMAKE_BINARY_DIR} and configure again "
        "(from build-mpv.cmd) to re-detect it.")
endif()
