cmake_minimum_required(VERSION 3.15)
project(AngelASIO LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

if(MSVC)
  # Runtime estatico: el driver se carga dentro del DAW y no debe depender del redistribuible de VC++.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W3 /EHsc)
endif()

add_library(AngelASIO SHARED
  src/dllmain.cpp
  src/driver.cpp
  src/engine.cpp
  src/config.cpp
  src/panel.cpp
  AngelASIO.def)
set_target_properties(AngelASIO PROPERTIES PREFIX "")
target_link_libraries(AngelASIO PRIVATE ole32 uuid avrt user32 gdi32 advapi32 comctl32)
if(MINGW)
  target_link_options(AngelASIO PRIVATE -static -static-libgcc -static-libstdc++)
endif()

add_executable(asio_smoke_test tools/smoke_test.cpp)
target_link_libraries(asio_smoke_test PRIVATE ole32 uuid)
if(MINGW)
  target_link_options(asio_smoke_test PRIVATE -static -static-libgcc -static-libstdc++)
endif()
