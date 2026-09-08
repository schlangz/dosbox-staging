target_sources(dosboxcommon PRIVATE
  bridge.cpp
  webserver.cpp
  cpu.cpp
  keyboard.cpp
  mouse.cpp
  memory.cpp
  dos.cpp
  dosbox.cpp
  screenshot.cpp)

target_link_libraries(dosboxcommon PRIVATE simde base64 httplib json)
