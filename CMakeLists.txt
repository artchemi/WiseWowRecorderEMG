cmake_minimum_required(VERSION 3.16)
project(RecorderEMG VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)

# Пути к библиотекам
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/libs/imgui)
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/libs/implot)
set(GLFW_DIR  ${CMAKE_SOURCE_DIR}/libs/glfw)
set(IIR_DIR   ${CMAKE_SOURCE_DIR}/libs/iir1)

# ---- ImGui ----
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ---- ImPlot ----
set(IMPLOT_SOURCES
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)

# ---- Iir (фильтры) ----
file(GLOB IIR_SOURCES ${IIR_DIR}/Iir/*.cpp)

# ---- Наш класс SensorEMG ----
set(SENSOR_SOURCES
    SensorEMG.cpp
)

set(SENSOR_HEADERS
    SensorEMG.h
)

# ---------- Первый исполняемый ----------
# add_executable(SingleRecorder
#     single.cpp
#     ${SENSOR_SOURCES}
#     ${SENSOR_HEADERS}
# )

# target_include_directories(SingleRecorder PRIVATE
#     ${IMGUI_DIR}
#     ${IMPLOT_DIR}
#     ${GLFW_DIR}/include
#     ${IIR_DIR}
#     ${CMAKE_SOURCE_DIR}   # чтобы видеть SensorEMG.h
# )

# target_link_libraries(SingleRecorder PRIVATE Threads::Threads)

# ---------- Второй исполняемый ----------
add_executable(SingleRecorderPlot
    single_plot.cpp
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
    ${IIR_SOURCES}
    ${SENSOR_SOURCES}
    ${SENSOR_HEADERS}
)

target_include_directories(SingleRecorderPlot PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
    ${GLFW_DIR}/include
    ${IIR_DIR}
    ${CMAKE_SOURCE_DIR}   # чтобы видеть SensorEMG.h
)

add_subdirectory(${GLFW_DIR})

target_link_libraries(SingleRecorderPlot PRIVATE
    Threads::Threads
    OpenGL::GL
    glfw
)
