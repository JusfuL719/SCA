cmake_minimum_required(VERSION 3.20)
project(sca_installer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
    main.cpp
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /guard:cf
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF /guard:cf /DEBUG:NONE"
    )
        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "SCAhost")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    kernel32
    advapi32
)
