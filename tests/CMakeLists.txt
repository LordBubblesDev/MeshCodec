add_executable(mc_test src/main.cpp)

target_link_libraries(mc_test PRIVATE MeshCodec)

include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.5
)

FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.9
)

FetchContent_MakeAvailable(glfw)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC glfw)

add_executable(mc_decompressor src/main_gui.cpp)

target_include_directories(mc_decompressor PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(mc_decompressor PRIVATE 
    MeshCodec 
    imgui 
    glfw
)

if(WIN32)
    target_compile_definitions(mc_decompressor PRIVATE _WIN32_WINNT=0x0601)
endif()

find_package(OpenGL REQUIRED)
target_link_libraries(mc_decompressor PRIVATE OpenGL::GL)