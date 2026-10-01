cmake_minimum_required(VERSION 3.20)

project(black-recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

option(BLACK_SHADOW "Wrap every translated function for the differential check against Unicorn (src/shadow.c)" OFF)
if(BLACK_SHADOW)
    set(BLACK_GEN_DIR "src/recomp/gen-shadow")
else()
    set(BLACK_GEN_DIR "src/recomp/gen")
endif()
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${BLACK_GEN_DIR}/*.c")

set(GAME_SOURCES
    src/main_posix.c
    src/recomp_manual.c
    src/recomp_natives.c
)

add_executable(${PROJECT_NAME}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE xemu_nv2a)
    if(XEMU_ANGLE_LIB_DIR)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XEMU_ANGLE_LIB_DIR}/libEGL.dylib" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/libEGL.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XEMU_ANGLE_LIB_DIR}/libGLESv2.dylib" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/libGLESv2.dylib"
            COMMAND install_name_tool -change
                "@executable_path/../Frameworks/libEGL.dylib" "@loader_path/libEGL.dylib"
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMAND install_name_tool -change
                "@executable_path/../Frameworks/libGLESv2.dylib" "@loader_path/libGLESv2.dylib"
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMAND codesign --force --sign - "$<TARGET_FILE:${PROJECT_NAME}>"
            VERBATIM)
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/${BLACK_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Cocoa"
        "-framework Foundation"
    )
endif()

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -O2)
endif()

if(BLACK_SHADOW)
    target_sources(${PROJECT_NAME} PRIVATE src/shadow.c)
    target_include_directories(${PROJECT_NAME} PRIVATE /opt/homebrew/include)
    target_link_libraries(${PROJECT_NAME} PRIVATE /opt/homebrew/lib/libunicorn.dylib)
endif()
