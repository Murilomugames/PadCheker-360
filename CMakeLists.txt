cmake_minimum_required(VERSION 3.25)
project(padcheck360 C)

add_executable(padcheck360 main.c)
set_target_properties(padcheck360 PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
)
target_compile_options(padcheck360 PRIVATE -Wall -Wextra -Werror)
xsdk_add_xex(padcheck360)
