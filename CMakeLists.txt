cmake_minimum_required(VERSION 3.28...3.30)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version} LANGUAGES C CXX)
include(compilerconfig)
include(defaults)
include(helpers)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
add_library(${CMAKE_PROJECT_NAME} MODULE)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/dock/vystrm-dock.cpp
  src/dock/vystrm.qrc
  src/auth/auth-manager.cpp
  src/auth/auth-manager.h
  src/auth/login-widget.cpp
  src/auth/login-widget.h
  src/bible/bible-assistant.cpp
  src/bible/bible-assistant.h
  src/qr_v4.h
)
if(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 shell32 advapi32 crypt32 iphlpapi gdi32 winmm)
elseif(APPLE)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/mac/plugin.c)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework Security")
else()
  message(FATAL_ERROR "VyStream OBS Dock supports Windows and macOS only")
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE VYSTRM_QT_DOCK=1)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src src/dock)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTORCC ON PREFIX "")
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME "obs-srt-camera")
