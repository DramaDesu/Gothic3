add_library(Script_Mcp SHARED)

target_link_libraries(Script_Mcp PRIVATE g3sdk::SDK ws2_32)

target_gothic_script_options(Script_Mcp)

target_include_directories(Script_Mcp PRIVATE src)

target_tree_sources(Script_Mcp
  BASE src
  PRIVATE
    Script_Mcp.cpp
    mcp_server.cpp
    mcp_server.h
    mcp_json.h
)
