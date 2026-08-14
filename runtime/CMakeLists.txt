# Runtime executable
# Vulkan rendering, SDL3 platform, runs compiled GameDefinition

#=============================================================================
# SHADER COMPILATION
#=============================================================================

# Find glslc (from Vulkan SDK or vcpkg shaderc)
find_program(GLSLC glslc HINTS 
    $ENV{VULKAN_SDK}/Bin
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shaderc
    C:/vcpkg/installed/x64-windows/tools/shaderc
)
if(NOT GLSLC)
    message(WARNING "glslc not found - shaders will not be compiled")
endif()

# Shader sources
set(SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/tile.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/tile.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/textbox.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/textbox.frag
)

# Output directory for compiled shaders
set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Compile shaders to SPIR-V
set(SHADER_BINARIES "")
if(GLSLC)
    foreach(SHADER ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${SHADER_OUTPUT}
            COMMAND ${GLSLC} -o ${SHADER_OUTPUT} ${SHADER}
            DEPENDS ${SHADER}
            COMMENT "Compiling shader ${SHADER_NAME}"
        )
        list(APPEND SHADER_BINARIES ${SHADER_OUTPUT})
    endforeach()
    
    add_custom_target(compile_shaders DEPENDS ${SHADER_BINARIES})
endif()

#=============================================================================
# BOOTSTRAP EXECUTABLE (minimal presentation test)
#=============================================================================

add_executable(enginemon_bootstrap
    main_bootstrap.cpp
    platform/sdl3_platform.cpp
    render/vulkan_bootstrap.cpp
)

target_include_directories(enginemon_bootstrap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(enginemon_bootstrap PRIVATE
    enginemon_engine
    Vulkan::Vulkan
    SDL3::SDL3
)

# Vulkan 1.3 for core dynamic rendering
target_compile_definitions(enginemon_bootstrap PRIVATE
    VK_API_VERSION_1_3
)


#=============================================================================
# TILE RENDERER EXECUTABLE (map rendering test)
#=============================================================================

add_executable(enginemon_tiles
    main_tiles.cpp
    platform/sdl3_platform.cpp
    render/vulkan_bootstrap.cpp
    render/vulkan_texture.cpp
    render/tile_renderer.cpp
    render/sprite_renderer.cpp
    render/textbox_renderer.cpp
)

target_include_directories(enginemon_tiles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(enginemon_tiles PRIVATE
    enginemon_engine
    Vulkan::Vulkan
    SDL3::SDL3
)

target_compile_definitions(enginemon_tiles PRIVATE
    VK_API_VERSION_1_3
    SHADER_DIR="${SHADER_OUTPUT_DIR}"
)

if(GLSLC)
    add_dependencies(enginemon_tiles compile_shaders)
endif()

# Full runtime executable (disabled until renderer is complete)
# add_executable(enginemon
#     main.cpp
#     ... full renderer files ...
# )
