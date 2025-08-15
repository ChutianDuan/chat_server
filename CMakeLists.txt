cmake_minimum_required(VERSION 3.16)
project(project_1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT           REQUIRED IMPORTED_TARGET libevent)
pkg_check_modules(LIBEVENT_PTHREADS  REQUIRED IMPORTED_TARGET libevent_pthreads)

include_directories(${CMAKE_SOURCE_DIR}/third_party)

option(BUILD_CHAT_SERVER    "Build chat_server"     ON)
option(BUILD_CHAT_CLIENT    "Build chat_client"     ON)
option(BUILD_CHAT_SERVER_LE "Build chat_server_le"  ON)

function(link_common tgt)
  target_link_libraries(${tgt}
    PRIVATE
      PkgConfig::LIBEVENT
      PkgConfig::LIBEVENT_PTHREADS
      Threads::Threads
  )
  target_compile_definitions(${tgt} PRIVATE _REENTRANT)
endfunction()

if (BUILD_CHAT_SERVER)
  add_executable(chat_server src/chat_server.cpp)
  link_common(chat_server)
endif()

if (BUILD_CHAT_CLIENT)
  add_executable(chat_client src/chat_client.cpp)
  link_common(chat_client)
endif()

if (BUILD_CHAT_SERVER_LE)
  add_executable(chat_server_le src/chat_server_le.cpp)
  link_common(chat_server_le)
endif()
