set(DEBUGGER_SRC
  callrec.cpp
  debugger.cpp
  debugger_disasm.cpp
  debugger_gui.cpp)

target_sources(dosboxcommon PRIVATE
  callrec.cpp
  debugger.cpp
  debugger_disasm.cpp
  debugger_gui.cpp)

target_link_libraries(dosboxcommon PRIVATE imgui)
