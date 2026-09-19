cmake_minimum_required(VERSION 3.25)

# vcpkg must be selected before project(), otherwise the first configure
# cannot use manifest mode to restore/find Qt6.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT CMAKE_TOOLCHAIN_FILE)
  set(_vcpkg_candidates
    "C:/Dev/vcpkg/scripts/buildsystems/vcpkg.cmake"
    "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    "$ENV{USERPROFILE}/vcpkg/scripts/buildsystems/vcpkg.cmake"
    "$ENV{USERPROFILE}/Documents/vcpkg/scripts/buildsystems/vcpkg.cmake"
  )

  foreach(_vcpkg_candidate IN LISTS _vcpkg_candidates)
    file(TO_CMAKE_PATH "${_vcpkg_candidate}" _vcpkg_candidate_norm)
    if(EXISTS "${_vcpkg_candidate_norm}")
      set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_candidate_norm}"
          CACHE FILEPATH "vcpkg toolchain file" FORCE)
      break()
    endif()
  endforeach()
endif()

if(CMAKE_TOOLCHAIN_FILE)
  message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
else()
  message(WARNING
    "vcpkg toolchain not found automatically; Qt6 must be discoverable by another route.")
endif()

project(OpenScopePcmEncoder VERSION 0.9.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


set(ASIO_SDK_ROOT "" CACHE PATH "Path to Steinberg ASIO SDK")

# ASIO SDK discovery. The SDK is not bundled with OpenScopePCMEncoder.
# The selected folder must contain common/asio.cpp and host/asiodrivers.cpp.
if(NOT ASIO_SDK_ROOT AND DEFINED ENV{ASIO_SDK_ROOT})
  file(TO_CMAKE_PATH "$ENV{ASIO_SDK_ROOT}" _asio_env_root)
  if(EXISTS "${_asio_env_root}/common/asio.cpp")
    set(ASIO_SDK_ROOT "${_asio_env_root}" CACHE PATH
        "Path to Steinberg ASIO SDK" FORCE)
  endif()
endif()

if(NOT ASIO_SDK_ROOT)
  set(_asio_candidates
    "C:/SDK/ASIOSDK"
    "C:/SDK/asiosdk"
    "C:/Dev/ASIOSDK"
    "C:/Dev/asiosdk"
    "${CMAKE_CURRENT_SOURCE_DIR}/ASIOSDK"
    "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk"
    "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ASIOSDK"
    "${CMAKE_CURRENT_SOURCE_DIR}/../asio"
    "$ENV{USERPROFILE}/Documents/asiosdk"
    "$ENV{USERPROFILE}/Documents/ASIOSDK"
    "$ENV{USERPROFILE}/Downloads/asiosdk"
    "$ENV{USERPROFILE}/Downloads/ASIOSDK"
  )
  foreach(_candidate IN LISTS _asio_candidates)
    file(TO_CMAKE_PATH "${_candidate}" _candidate_norm)
    if(EXISTS "${_candidate_norm}/common/asio.cpp" AND
       EXISTS "${_candidate_norm}/host/asiodrivers.cpp" AND
       EXISTS "${_candidate_norm}/host/pc/asiolist.cpp")
      set(ASIO_SDK_ROOT "${_candidate_norm}" CACHE PATH
          "Path to Steinberg ASIO SDK" FORCE)
      break()
    endif()


  endforeach()
endif()

if(NOT ASIO_SDK_ROOT)
  message(FATAL_ERROR
    "Steinberg ASIO SDK was not found.\n"
    "Tried common locations including C:/SDK/ASIOSDK and project-local ASIOSDK/.\n"
    "Set ASIO_SDK_ROOT to the SDK root (containing common/, host/ and host/pc/).")
endif()

message(STATUS "Using ASIO SDK: ${ASIO_SDK_ROOT}")

set(DECKLINK_SDK_ROOT "" CACHE PATH "Path to Blackmagic Desktop Video SDK")

# Friendly DeckLink SDK discovery.
# Priority:
#   1. Explicit CMake cache/preset: -DDECKLINK_SDK_ROOT=...
#   2. Environment variable DECKLINK_SDK_ROOT
#   3. A few common/sibling locations near the source tree.
if(NOT DECKLINK_SDK_ROOT AND DEFINED ENV{DECKLINK_SDK_ROOT})
  file(TO_CMAKE_PATH "$ENV{DECKLINK_SDK_ROOT}" _decklink_env_root)
  if(EXISTS "${_decklink_env_root}/Win/include/DeckLinkAPI.idl")
    set(DECKLINK_SDK_ROOT "${_decklink_env_root}" CACHE PATH
        "Path to Blackmagic Desktop Video SDK" FORCE)
  endif()
endif()

if(NOT DECKLINK_SDK_ROOT)
  set(_decklink_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/../DeckLink SDK 16.0"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Blackmagic DeckLink SDK 16.0"
    "${CMAKE_CURRENT_SOURCE_DIR}/../DeckLink SDK"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Blackmagic DeckLink SDK"
    "$ENV{USERPROFILE}/Documents/DeckLink SDK 16.0"
    "$ENV{USERPROFILE}/Documents/Blackmagic DeckLink SDK 16.0"
    "$ENV{USERPROFILE}/Downloads/DeckLink SDK 16.0"
    "$ENV{USERPROFILE}/Downloads/Blackmagic DeckLink SDK 16.0"
    "C:/Program Files/Blackmagic Design/DeckLink SDK"
  )

  foreach(_candidate IN LISTS _decklink_candidates)
    file(TO_CMAKE_PATH "${_candidate}" _candidate_norm)
    if(EXISTS "${_candidate_norm}/Win/include/DeckLinkAPI.idl")
      set(DECKLINK_SDK_ROOT "${_candidate_norm}" CACHE PATH
          "Path to Blackmagic Desktop Video SDK" FORCE)
      break()
    endif()
  endforeach()
endif()

if(NOT DECKLINK_SDK_ROOT)
  message(FATAL_ERROR
    "Blackmagic DeckLink SDK was not found automatically.\n"
    "Set DECKLINK_SDK_ROOT once, either as a CMake variable or environment variable.\n"
    "The selected folder must contain Win/include/DeckLinkAPI.idl.")
endif()

if(NOT EXISTS "${DECKLINK_SDK_ROOT}/Win/include/DeckLinkAPI.idl")
  message(FATAL_ERROR
    "DeckLinkAPI.idl not found under ${DECKLINK_SDK_ROOT}/Win/include")
endif()

message(STATUS "Using DeckLink SDK: ${DECKLINK_SDK_ROOT}")

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)
qt_standard_project_setup()

# Explicit for clarity; MainWindow uses Q_OBJECT.
set(CMAKE_AUTOMOC ON)

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/decklink_generated")
set(DECKLINK_IDL "${DECKLINK_SDK_ROOT}/Win/include/DeckLinkAPI.idl")
add_custom_command(
  OUTPUT "${GEN_DIR}/DeckLinkAPI_h.h" "${GEN_DIR}/DeckLinkAPI_i.c"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
  COMMAND midl /nologo /env x64 /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c /out "${GEN_DIR}" "${DECKLINK_IDL}"
  DEPENDS "${DECKLINK_IDL}" VERBATIM)
add_custom_target(DeckLinkApiGenerated DEPENDS "${GEN_DIR}/DeckLinkAPI_h.h" "${GEN_DIR}/DeckLinkAPI_i.c")

qt_add_executable(OpenScopePcmEncoder
  src/main.cpp
  src/MainWindow.h src/MainWindow.cpp
  src/PpmMeter.h src/PpmMeter.cpp
  src/BufferNudgeController.h src/BufferNudgeController.cpp
  src/AudioAsrc.h src/AudioAsrc.cpp
  src/HamAudioAsrc.h src/HamAudioAsrc.cpp
  src/AsioCapture.h src/AsioCapture.cpp
  src/Pcm16VideoEncoder.h src/Pcm16VideoEncoder.cpp
  src/HamPcmV2Encoder.h src/HamPcmV2Encoder.cpp
  src/DeckLinkApiWin.h
  src/DeckLinkPalOutput.h src/DeckLinkPalOutput.cpp
  "${GEN_DIR}/DeckLinkAPI_i.c"
  "${ASIO_SDK_ROOT}/common/asio.cpp"
  "${ASIO_SDK_ROOT}/host/asiodrivers.cpp"
  "${ASIO_SDK_ROOT}/host/pc/asiolist.cpp")

# The Steinberg ASIO SDK Windows host helper sources are legacy ANSI/COM code.
# Compile only those translation units in the environment they expect:
#   - no UNICODE mapping for Win32 registry APIs (asiolist.cpp uses char buffers)
#   - COM declarations available before iasiodrv.h
#   - legacy `interface` macro explicitly defined
set(_asio_sdk_sources
    "${ASIO_SDK_ROOT}/common/asio.cpp"
    "${ASIO_SDK_ROOT}/host/asiodrivers.cpp"
    "${ASIO_SDK_ROOT}/host/pc/asiolist.cpp"
)

set_source_files_properties(${_asio_sdk_sources}
    PROPERTIES
        COMPILE_DEFINITIONS "interface=struct;IEEE754_64FLOAT=1"
        COMPILE_OPTIONS "/UUNICODE;/U_UNICODE;/FIWindows.h;/FIObjbase.h"
)

add_dependencies(OpenScopePcmEncoder DeckLinkApiGenerated)

# This is a GUI application. Do not create a console window when launching
# the Release executable directly from Explorer or the deploy directory.
set_target_properties(OpenScopePcmEncoder PROPERTIES
  WIN32_EXECUTABLE TRUE)

target_include_directories(OpenScopePcmEncoder PRIVATE
  "${GEN_DIR}"
  "${ASIO_SDK_ROOT}/common"
  "${ASIO_SDK_ROOT}/host"
  "${ASIO_SDK_ROOT}/host/pc")
target_link_libraries(OpenScopePcmEncoder PRIVATE Qt6::Widgets ole32 uuid avrt)

# Deploy the Qt runtime next to the executable so the app can be launched
# directly from Visual Studio/build output. In particular this installs:
#   platforms/qwindows.dll
# plus the required Qt runtime DLLs.
# Find windeployqt. With vcpkg, the deployment tool is normally under
# installed/<triplet>/tools/Qt6/bin rather than beside the Qt libraries.
set(_windeployqt_hints)

if(TARGET Qt6::qmake)
  get_target_property(_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
  if(_qt_qmake_executable)
    get_filename_component(_qt_bin_dir "${_qt_qmake_executable}" DIRECTORY)
    list(APPEND _windeployqt_hints "${_qt_bin_dir}")
  endif()
endif()

if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  list(APPEND _windeployqt_hints
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/Qt6/bin"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/Qt6")
endif()

# Known OpenScope development vcpkg location.
list(APPEND _windeployqt_hints
  "C:/Dev/vcpkg/installed/x64-windows/tools/Qt6/bin"
  "C:/Dev/vcpkg/installed/x64-windows/tools/Qt6")

find_program(WINDEPLOYQT_EXECUTABLE
  NAMES windeployqt.exe windeployqt windeployqt6.exe windeployqt6
  HINTS ${_windeployqt_hints})

if(WINDEPLOYQT_EXECUTABLE)
  message(STATUS "Using windeployqt: ${WINDEPLOYQT_EXECUTABLE}")

  add_custom_command(
    TARGET OpenScopePcmEncoder
    POST_BUILD
    COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --no-translations
            --no-system-d3d-compiler
            "$<TARGET_FILE:OpenScopePcmEncoder>"
    COMMENT "Deploying Qt runtime for OpenScopePcmEncoder"
    VERBATIM)
else()
  message(WARNING
    "windeployqt was not found. The encoder will still build, but Qt runtime "
    "DLLs/platforms/qwindows.dll must be copied before running it.")
endif()


# Explicitly deploy the Windows QPA platform plugin.
# Some vcpkg/Visual Studio combinations copy the Qt runtime DLLs but omit
# platforms/qwindows[d].dll, causing:
#   Could not find the Qt platform plugin "windows"
#
# Prefer Qt's imported plugin target. This generator expression selects the
# correct Debug/Release plugin automatically.
if(TARGET Qt6::QWindowsIntegrationPlugin)
  add_custom_command(
    TARGET OpenScopePcmEncoder
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:OpenScopePcmEncoder>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE_DIR:OpenScopePcmEncoder>/platforms/$<TARGET_FILE_NAME:Qt6::QWindowsIntegrationPlugin>"
    COMMENT "Copying Qt Windows platform plugin"
    VERBATIM)
else()
  message(WARNING
    "Qt6::QWindowsIntegrationPlugin target not available; relying on windeployqt for qwindows plugin.")
endif()

# Put a minimal qt.conf beside the exe so Qt searches the local plugin tree.
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/qt.conf"
  CONTENT "[Paths]\nPlugins=.\n")

add_custom_command(
  TARGET OpenScopePcmEncoder
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_BINARY_DIR}/qt.conf"
          "$<TARGET_FILE_DIR:OpenScopePcmEncoder>/qt.conf"
  COMMENT "Installing qt.conf"
  VERBATIM)

target_compile_definitions(OpenScopePcmEncoder PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN IEEE754_64FLOAT=1)
