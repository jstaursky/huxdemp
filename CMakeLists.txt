cmake_minimum_required(VERSION 3.1)
project(huxdemp)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
add_definitions(-DVERSION="1.0.0")
file(GLOB LUA_EMBEDDED_SCRIPTS RELATIVE ${CMAKE_BINARY_DIR} "${CMAKE_SOURCE_DIR}/resources/*.lua")
message(STATUS ".lua files ${LUA_EMBEDDED_SCRIPTS}")

set(embedc_args "")
foreach(lua_script ${LUA_EMBEDDED_SCRIPTS})
  get_filename_component(base_name ${lua_script} NAME_WLE)
  list(APPEND embedc_args "${lua_script}:${base_name}")
endforeach()

string(REPLACE ";" " " EMBEDC_ARGS_FIXED "${embedc_args}")

add_executable(embedc tools/embedc.c)

add_custom_command(
  OUTPUT builtin.c
  COMMAND embedc ARGS "${EMBEDC_ARGS_FIXED}" > builtin.c
  MAIN_DEPENDENCY ${LUA_EMBEDDED_SCRIPTS}
  DEPENDS embedc
  COMMENT "running embedc against .lua scripts"
  VERBATIM
)

find_library(LUALIB lua)
find_library(MATHLIB m)
find_library(DL dl)

add_executable(huxdemp src/main.c)
add_custom_target(generate_builtin_src DEPENDS builtin.c)
add_dependencies(huxdemp generate_builtin_src)
target_link_libraries(huxdemp ${LUALIB} ${MATHLIB} ${DL})
