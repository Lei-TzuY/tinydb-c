cmake_minimum_required(VERSION 3.10)
project(TinyDB C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED True)

# Enable all warnings
if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

file(GLOB SOURCES "src/*.c")

add_executable(tinydb ${SOURCES})
