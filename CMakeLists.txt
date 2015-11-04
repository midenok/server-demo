cmake_minimum_required(VERSION 3.2)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_executable(server-demo main.cc main_opts.c threads.cc)
target_link_libraries(server-demo -lopts -lpthread -lev)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++11" )
add_custom_command(OUTPUT main_opts.c main_opts.h COMMAND autogen ${CMAKE_CURRENT_SOURCE_DIR}/main_opts.def MAIN_DEPENDENCY main_opts.def)
# use `autoopts-config ldflags` instead of -lopts
