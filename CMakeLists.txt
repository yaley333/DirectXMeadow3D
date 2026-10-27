cmake_minimum_required(VERSION 3.24)
project(LiminalMeadow LANGUAGES CXX)

add_executable(LiminalMeadow WIN32 src/main.cpp)
target_compile_features(LiminalMeadow PRIVATE cxx_std_17)
target_compile_definitions(LiminalMeadow PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(LiminalMeadow PRIVATE d3d11 dxgi d3dcompiler winmm)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/music/music.mp3")
    add_custom_command(TARGET LiminalMeadow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LiminalMeadow>/music"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/music/music.mp3"
            "$<TARGET_FILE_DIR:LiminalMeadow>/music/music.mp3"
        VERBATIM)
endif()

if(MSVC)
    target_compile_options(LiminalMeadow PRIVATE /W4 /permissive- /EHsc)
endif()
