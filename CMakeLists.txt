cmake_minimum_required(VERSION 3.10)
project(SkiaTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "-O3 -Wall -Wextra -march=x86-64-v3")
set(CMAKE_CXX_FLAGS_DEBUG "-g -Wall -Wextra")

# Find system packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(OPENVR REQUIRED openvr)

# Point to your built Skia (adjust path if Skia is elsewhere)
#set(SKIA_DIR "${CMAKE_SOURCE_DIR}/skia/skia")
set(SKIA_DIR "/home/spacy/src/skia")
set(SKIA_OUT_DIR "${SKIA_DIR}/out/Static")

# Include Skia headers
include_directories(
    ${SKIA_DIR}
    ${SKIA_DIR}/include
    ${SKIA_DIR}/include/core
    ${SKIA_DIR}/include/effects
    ${SKIA_DIR}/include/gpu
    ${SKIA_DIR}/include/config
    ${SKIA_DIR}/include/utils
    ${SKIA_DIR}/include/ports
    ${SKIA_DIR}/modules/skottie/include  # If needed for animations
)

# Add your executable
add_executable(app main.cpp)

# Link libraries
target_link_libraries(app
    PRIVATE
    ${SKIA_OUT_DIR}/libskia.a  # Static Skia lib
    ${GLFW_LIBRARIES}
    #OpenGL::GL
    dl
    pthread
    jpeg
    freetype
    z
    png
    fontconfig
    webp
    webpmux
    webpdemux
    openvr_api
)

# For runtime shared libs if any
target_link_directories(app PRIVATE ${SKIA_OUT_DIR})

find_package(Vulkan)
target_link_libraries(app PRIVATE Vulkan::Vulkan)