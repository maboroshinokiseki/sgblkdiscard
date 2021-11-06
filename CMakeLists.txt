cmake_minimum_required(VERSION 3.1.5)

project(sgblkdiscard VERSION 0.1.0)

set(CMAKE_C_STANDARD 99)

configure_file(sgblkdiscard_config.h.in sgblkdiscard_config.h)

add_executable(${PROJECT_NAME} sgblkdiscard.c utils.c)

target_include_directories(${PROJECT_NAME} PUBLIC
                            "${PROJECT_BINARY_DIR}"
                            )
find_library(myblkid blkid)
if(myblkid)
    add_compile_definitions(HAVE_LIBBLKID)
    target_link_libraries(${PROJECT_NAME} ${myblkid})
endif()
