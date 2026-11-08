cmake_minimum_required(VERSION 3.25)

project(LiteCap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  message(FATAL_ERROR "LiteCap wird nur mit MSVC (Visual Studio 2022) gebaut.")
endif()

# Dynamische CRT (/MD) passend zum Triplet x64-windows-static-md (Libs statisch gelinkt)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(nlohmann_json CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

add_executable(litecap WIN32
  src/main.cpp
  src/app/App.cpp
  src/app/TrayIcon.cpp
  src/app/Hotkeys.cpp
  src/config/Config.cpp
  src/capture/GpuDevice.cpp
  src/capture/CaptureTarget.cpp
  src/capture/WgcCapture.cpp
  src/capture/ColorConverter.cpp
  src/audio/WasapiSource.cpp
  src/audio/AudioTrack.cpp
  src/encode/FFmpeg.cpp
  src/encode/EncoderInfo.cpp
  src/encode/VideoEncoder.cpp
  src/output/Muxer.cpp
  src/output/FileNamer.cpp
  src/output/Transcoder.cpp
  src/output/PostProcessor.cpp
  src/pipeline/ReplayBuffer.cpp
  src/upload/CredentialStore.cpp
  src/upload/Http.cpp
  src/upload/GoogleAuth.cpp
  src/upload/YouTubeUploader.cpp
  src/tools/Commands.cpp
  src/pipeline/CaptureSession.cpp
  src/pipeline/Recorder.cpp
  src/tools/SelfTest.cpp
  src/util/Log.cpp
  src/util/Strings.cpp
  src/util/Paths.cpp
  res/litecap.rc
)

target_include_directories(litecap PRIVATE src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(litecap PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_compile_definitions(litecap PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00   # Windows 10
  LITECAP_VERSION="${PROJECT_VERSION}"
)

target_compile_options(litecap PRIVATE
  /W4 /permissive- /utf-8 /Zc:__cplusplus /MP /EHsc
  $<$<CONFIG:Release>:/O2 /GL /Gy>
)
target_link_options(litecap PRIVATE
  /MANIFEST:NO   # Manifest kommt aus res/litecap.rc
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

target_link_libraries(litecap PRIVATE
  nlohmann_json::nlohmann_json
  ${FFMPEG_LIBRARIES}
  # Windows
  d3d11 dxgi dxguid windowsapp
  avrt dwmapi ole32 shell32 shlwapi user32 gdi32 advapi32 comctl32
  mfplat mfuuid mfreadwrite strmiids
  winhttp crypt32 bcrypt ws2_32 secur32
)
