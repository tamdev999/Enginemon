// runtime/render/tile_renderer.cpp
// 8×8 tile-based map renderer implementation
//
// Renders maps by expanding blocks to tile instances.
// Uses batched rendering for efficiency (one/few draws, not per-tile).

#include "render/tile_renderer.hpp"
#include "render/vulkan_bootstrap.hpp"
#include <fstream>
#include <cstring>
#include <iostream>
#include <algorithm>

#ifndef SHADER_DIR
#define SHADER_DIR "."
#endif

namespace enginemon {

TileRenderer::TileRenderer() = default;

TileRenderer::~TileRenderer() {
    destroy();
}

// ─── PreparedMapGeometry move semantics ────────────────────────────────────

TileRenderer::PreparedMapGeometry::PreparedMapGeometry(PreparedMapGeometry&& o) noexcept
    : device(o.device), vertex_buffer(o.vertex_buffer), vertex_memory(o.vertex_memory),
      index_buffer(o.index_buffer), index_memory(o.index_memory),
      index_count(o.index_count), map_width(o.map_width), map_height(o.map_height),
      valid(o.valid)
{
    o.device = VK_NULL_HANDLE;
    o.vertex_buffer = VK_NULL_HANDLE; o.vertex_memory = VK_NULL_HANDLE;
    o.index_buffer  = VK_NULL_HANDLE; o.index_memory  = VK_NULL_HANDLE;
    o.valid = false;
}

TileRenderer::PreparedMapGeometry& TileRenderer::PreparedMapGeometry::operator=(PreparedMapGeometry&& o) noexcept {
    if (this != &o) {
        // Destroy any resources we own
        if (vertex_buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, vertex_buffer, nullptr); }
        if (vertex_memory != VK_NULL_HANDLE) { vkFreeMemory(device, vertex_memory, nullptr); }
        if (index_buffer  != VK_NULL_HANDLE) { vkDestroyBuffer(device, index_buffer, nullptr); }
        if (index_memory  != VK_NULL_HANDLE) { vkFreeMemory(device, index_memory, nullptr); }
        device = o.device; vertex_buffer = o.vertex_buffer; vertex_memory = o.vertex_memory;
        index_buffer = o.index_buffer; index_memory = o.index_memory;
        index_count = o.index_count; map_width = o.map_width; map_height = o.map_height;
        valid = o.valid;
        o.device = VK_NULL_HANDLE;
        o.vertex_buffer = VK_NULL_HANDLE; o.vertex_memory = VK_NULL_HANDLE;
        o.index_buffer  = VK_NULL_HANDLE; o.index_memory  = VK_NULL_HANDLE;
        o.valid = false;
    }
    return *this;
}

TileRenderer::PreparedMapGeometry::~PreparedMapGeometry() {
    if (device == VK_NULL_HANDLE) return;
    if (vertex_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, vertex_buffer, nullptr);
    if (vertex_memory != VK_NULL_HANDLE) vkFreeMemory(device, vertex_memory, nullptr);
    if (index_buffer  != VK_NULL_HANDLE) vkDestroyBuffer(device, index_buffer, nullptr);
    if (index_memory  != VK_NULL_HANDLE) vkFreeMemory(device, index_memory, nullptr);
}

void TileRenderer::PreparedMapGeometry::release() {
    // Nullify handles so the destructor doesn't free them
    // (ownership transferred to the renderer's live members)
    device = VK_NULL_HANDLE;
    vertex_buffer = VK_NULL_HANDLE; vertex_memory = VK_NULL_HANDLE;
    index_buffer  = VK_NULL_HANDLE; index_memory  = VK_NULL_HANDLE;
    valid = false;
}

// ─── prepare_tileset ────────────────────────────────────────────────────────

std::optional<TileRenderer::PreparedTileset> TileRenderer::prepare_tileset(
    VulkanBootstrap& vk, const RuntimeTileset& tileset, PaletteRow active_row)
{
    // Resolve palette set
    const RuntimePaletteSet* palette_set = nullptr;
    if (tileset.fixed_special_palette) {
        palette_set = &(*tileset.fixed_special_palette);
    } else {
        palette_set = &tileset.standard_palette_rows[static_cast<size_t>(active_row)];
    }

    TileAtlas atlas = TileAtlas::from_tileset_with_palette(tileset, *palette_set);
    if (atlas.pixels.empty()) return std::nullopt;

    PreparedTileset out;
    if (!out.texture.create(vk, atlas.width, atlas.height, atlas.pixels.data())) {
        return std::nullopt;
    }

    out.tile_uvs   = atlas.tile_uvs;
    out.blocks     = tileset.blocks;
    out.palette_map = tileset.palette_map;

    // Allocate descriptor set against the prepared texture — NOT live descriptor_pool_
    vkResetDescriptorPool(device_, descriptor_pool_, 0);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptor_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &descriptor_layout_;

    if (vkAllocateDescriptorSets(device_, &alloc, &out.descriptor_set) != VK_SUCCESS) {
        return std::nullopt;  // out.texture destroyed by PreparedTileset dtor
    }

    VkDescriptorImageInfo img{};
    img.sampler = out.texture.sampler();
    img.imageView = out.texture.view();
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = out.descriptor_set;
    wr.dstBinding = 0;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.descriptorCount = 1;
    wr.pImageInfo = &img;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);

    out.valid = true;
    return out;
}

// ─── prepare_map ────────────────────────────────────────────────────────────

std::optional<TileRenderer::PreparedMapGeometry> TileRenderer::prepare_map(
    VulkanBootstrap& vk, const RuntimeMap& map,
    const std::vector<TileUV>& tileset_uvs,
    const std::vector<uint8_t>& tileset_pal_map,
    const std::vector<RuntimeBlock>& tileset_blocks)
{
    // Build geometry using the STAGED tileset data (not live tile_uvs_/palette_map_)
    const float tile_size = static_cast<float>(config_.tile_size);
    std::vector<TileVertex> vertices;
    std::vector<uint32_t>   indices;
    vertices.reserve(static_cast<size_t>(map.width) * map.height * 16 * 4);
    indices.reserve(static_cast<size_t>(map.width) * map.height * 16 * 6);

    for (uint8_t by = 0; by < map.height; ++by) {
        for (uint8_t bx = 0; bx < map.width; ++bx) {
            uint8_t block_idx = map.blocks[by * map.width + bx];
            if (block_idx >= tileset_blocks.size()) block_idx = 0;
            const RuntimeBlock& block = tileset_blocks[block_idx];
            for (int ty = 0; ty < 4; ++ty) {
                for (int tx = 0; tx < 4; ++tx) {
                    uint16_t tile_id = block.tile_ids[ty * 4 + tx];
                    TileUV uv{};
                    if (!tileset_uvs.empty() && tile_id < tileset_uvs.size())
                        uv = tileset_uvs[tile_id];
                    float pal_id = 0.0f;
                    if (!tileset_pal_map.empty() && tile_id < tileset_pal_map.size())
                        pal_id = static_cast<float>(tileset_pal_map[tile_id]);
                    float px = (bx * 4 + tx) * tile_size;
                    float py = (by * 4 + ty) * tile_size;
                    uint32_t base = static_cast<uint32_t>(vertices.size());
                    vertices.push_back({px,             py,             uv.u0, uv.v0, pal_id});
                    vertices.push_back({px + tile_size, py,             uv.u1, uv.v0, pal_id});
                    vertices.push_back({px + tile_size, py + tile_size, uv.u1, uv.v1, pal_id});
                    vertices.push_back({px,             py + tile_size, uv.u0, uv.v1, pal_id});
                    indices.push_back(base+0); indices.push_back(base+1); indices.push_back(base+2);
                    indices.push_back(base+0); indices.push_back(base+2); indices.push_back(base+3);
                }
            }
        }
    }

    // Allocate GPU buffers into local handles — live buffers untouched
    PreparedMapGeometry out;
    out.device = device_;

    VkDeviceSize vsize = vertices.size() * sizeof(TileVertex);
    VkDeviceSize isize = indices.size()  * sizeof(uint32_t);

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto alloc_buf = [&](VkDeviceSize sz, VkBufferUsageFlags usage,
                         VkBuffer& buf, VkDeviceMemory& mem) -> bool {
        bi.size = sz; bi.usage = usage;
        if (vkCreateBuffer(device_, &bi, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device_, buf, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_memory_type(vk.physical_device(), mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) {
            vkDestroyBuffer(device_, buf, nullptr); buf = VK_NULL_HANDLE; return false;
        }
        vkBindBufferMemory(device_, buf, mem, 0);
        return true;
    };

    if (!alloc_buf(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, out.vertex_buffer, out.vertex_memory))
        return std::nullopt;  // PreparedMapGeometry dtor cleans up
    if (!alloc_buf(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, out.index_buffer, out.index_memory))
        return std::nullopt;

    // Copy data
    void* data;
    vkMapMemory(device_, out.vertex_memory, 0, vsize, 0, &data);
    std::memcpy(data, vertices.data(), vsize);
    vkUnmapMemory(device_, out.vertex_memory);

    vkMapMemory(device_, out.index_memory, 0, isize, 0, &data);
    std::memcpy(data, indices.data(), isize);
    vkUnmapMemory(device_, out.index_memory);

    out.index_count = static_cast<uint32_t>(indices.size());
    out.map_width   = map.width;
    out.map_height  = map.height;
    out.valid = true;
    return out;
}

// ─── commit (tile + map) ─────────────────────────────────────────────────────

void TileRenderer::commit(PreparedTileset&& tile, PreparedMapGeometry&& map) {
    // Destroy old live resources
    destroy_buffers();
    // tile_texture_ destructor runs on move assignment
    tile_texture_   = std::move(tile.texture);
    tile_uvs_       = std::move(tile.tile_uvs);
    blocks_         = std::move(tile.blocks);
    palette_map_    = std::move(tile.palette_map);
    descriptor_set_ = tile.descriptor_set;

    vertex_buffer_ = map.vertex_buffer;
    vertex_memory_ = map.vertex_memory;
    index_buffer_  = map.index_buffer;
    index_memory_  = map.index_memory;
    index_count_   = map.index_count;
    map_width_     = map.map_width;
    map_height_    = map.map_height;
    map.release();  // ownership transferred — prevent dtor from double-freeing
}

void TileRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(device_);
    
    destroy_buffers();
    tile_texture_.destroy();
    
    if (pipeline_) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_layout_) {
        vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
        descriptor_layout_ = VK_NULL_HANDLE;
    }
    
    device_ = VK_NULL_HANDLE;
}

void TileRenderer::destroy_buffers() {
    if (vertex_buffer_) {
        vkDestroyBuffer(device_, vertex_buffer_, nullptr);
        vertex_buffer_ = VK_NULL_HANDLE;
    }
    if (vertex_memory_) {
        vkFreeMemory(device_, vertex_memory_, nullptr);
        vertex_memory_ = VK_NULL_HANDLE;
    }
    if (index_buffer_) {
        vkDestroyBuffer(device_, index_buffer_, nullptr);
        index_buffer_ = VK_NULL_HANDLE;
    }
    if (index_memory_) {
        vkFreeMemory(device_, index_memory_, nullptr);
        index_memory_ = VK_NULL_HANDLE;
    }
    index_count_ = 0;
}

uint32_t TileRenderer::find_memory_type(VkPhysicalDevice physical_device,
                                         uint32_t type_filter,
                                         VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkShaderModule TileRenderer::create_shader_module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = size;
    create_info.pCode = code;
    
    VkShaderModule module;
    if (vkCreateShaderModule(device_, &create_info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

bool TileRenderer::initialize(VulkanBootstrap& vk, const TileRendererConfig& config) {
    device_ = vk.device();
    config_ = config;
    
    return create_pipeline(vk);
}

bool TileRenderer::create_pipeline(VulkanBootstrap& vk) {
    // Load shader files
    auto load_shader = [](const std::string& path) -> std::vector<uint32_t> {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "Failed to open shader: " << path << "\n";
            return {};
        }
        size_t size = file.tellg();
        file.seekg(0);
        std::vector<uint32_t> buffer((size + 3) / 4);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        return buffer;
    };
    
    std::string shader_dir = SHADER_DIR;
    auto vert_code = load_shader(shader_dir + "/tile.vert.spv");
    auto frag_code = load_shader(shader_dir + "/tile.frag.spv");
    
    if (vert_code.empty() || frag_code.empty()) {
        std::cerr << "Failed to load shaders from " << shader_dir << "\n";
        return false;
    }
    
    VkShaderModule vert_module = create_shader_module(
        vert_code.data(), vert_code.size() * sizeof(uint32_t));
    VkShaderModule frag_module = create_shader_module(
        frag_code.data(), frag_code.size() * sizeof(uint32_t));
    
    if (!vert_module || !frag_module) {
        if (vert_module) vkDestroyShaderModule(device_, vert_module, nullptr);
        if (frag_module) vkDestroyShaderModule(device_, frag_module, nullptr);
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName = "main";
    
    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName = "main";
    
    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};
    
    // Vertex input: position (vec2) + UV (vec2) + pal_id (float)
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(TileVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(TileVertex, x);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(TileVertex, u);
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32_SFLOAT;
    attrs[2].offset = offsetof(TileVertex, pal_id);
    
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attrs;
    
    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport/scissor
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;
    
    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    
    // Multisampling (disabled)
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Color blending (alpha blend)
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;

    // Descriptor set layout (for tile texture)
    VkDescriptorSetLayoutBinding sampler_binding{};
    sampler_binding.binding = 0;
    sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_binding.descriptorCount = 1;
    sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &sampler_binding;
    
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_layout_) 
        != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return false;
    }
    
    // Push constant range (camera + screen size)
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(float) * 4;  // camera.xy + screenSize.xy
    
    // Pipeline layout
    VkPipelineLayoutCreateInfo layout_create{};
    layout_create.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_create.setLayoutCount = 1;
    layout_create.pSetLayouts = &descriptor_layout_;
    layout_create.pushConstantRangeCount = 1;
    layout_create.pPushConstantRanges = &push_range;
    
    if (vkCreatePipelineLayout(device_, &layout_create, nullptr, &pipeline_layout_) 
        != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return false;
    }

    // Dynamic rendering format (Vulkan 1.3)
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    VkFormat color_format = vk.swapchain_format();
    rendering_info.pColorAttachmentFormats = &color_format;
    
    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout_;
    
    VkResult result = vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);
    
    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);
    
    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline\n";
        return false;
    }
    
    // Create descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;
    
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) 
        != VK_SUCCESS) {
        return false;
    }
    
    return true;
}

bool TileRenderer::set_tileset(VulkanBootstrap& vk, const RuntimeTileset& tileset, PaletteRow active_row) {
    // F4-renderer: Stage new texture locally.
    // Do NOT touch tile_texture_ until the new texture is fully ready.
    // If creation fails, tile_texture_ (and the live renderer) remain valid.
    
    // Resolve the palette set to use:
    // - If tileset has fixed_special_palette, use it
    // - Otherwise, use standard_palette_rows[active_row]
    const RuntimePaletteSet* palette_set = nullptr;
    if (tileset.fixed_special_palette) {
        palette_set = &(*tileset.fixed_special_palette);
    } else {
        palette_set = &tileset.standard_palette_rows[static_cast<size_t>(active_row)];
    }
    
    // Generate tile atlas with pre-resolved colors using the selected palette set
    TileAtlas atlas = TileAtlas::from_tileset_with_palette(tileset, *palette_set);
    
    if (atlas.pixels.empty()) {
        std::cerr << "[TILE RENDERER] Failed to generate tile atlas\n";
        return false;  // tile_texture_ unchanged
    }
    
    // Create new texture into a local VulkanTexture — NOT into tile_texture_ yet.
    // If this fails the existing tile_texture_ is still intact.
    VulkanTexture new_texture;
    if (!new_texture.create(vk, atlas.width, atlas.height, atlas.pixels.data())) {
        std::cerr << "[TILE RENDERER] Failed to create tile texture\n";
        return false;  // tile_texture_ unchanged
    }
    
    // Update cached data BEFORE committing the texture
    std::vector<TileUV>     new_tile_uvs  = atlas.tile_uvs;
    std::vector<RuntimeBlock> new_blocks  = tileset.blocks;
    std::vector<uint8_t>    new_pal_map   = tileset.palette_map;
    
    // Reset descriptor pool before allocating new set
    vkResetDescriptorPool(device_, descriptor_pool_, 0);
    
    // Allocate descriptor set using the new texture's view
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_layout_;
    
    VkDescriptorSet new_descriptor_set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &alloc_info, &new_descriptor_set) != VK_SUCCESS) {
        // new_texture destroyed by its destructor — tile_texture_ unchanged
        return false;
    }
    
    // Update descriptor with new texture's image info
    VkDescriptorImageInfo image_info{};
    image_info.sampler = new_texture.sampler();
    image_info.imageView = new_texture.view();
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = new_descriptor_set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    
    // ALL preparation succeeded — commit: swap new texture into live state.
    // The old tile_texture_ is destroyed by VulkanTexture's move assignment.
    tile_texture_ = std::move(new_texture);
    tile_uvs_     = std::move(new_tile_uvs);
    blocks_       = std::move(new_blocks);
    palette_map_  = std::move(new_pal_map);
    descriptor_set_ = new_descriptor_set;
    
    return true;
}

// Palette resolution helper - implements Crystal's proven palette selection
PaletteRow TileRenderer::resolve_palette_row(
    Environment env, 
    PalettePolicy policy,
    PaletteRow rtc_time)
{
    // First, resolve effective time from policy
    PaletteRow effective_time;
    switch (policy) {
        case PalettePolicy::Auto:
            effective_time = rtc_time;  // Use RTC
            break;
        case PalettePolicy::Day:
            effective_time = PaletteRow::Day;
            break;
        case PalettePolicy::Nite:
            effective_time = PaletteRow::Nite;
            break;
        case PalettePolicy::Morn:
            effective_time = PaletteRow::Morn;
            break;
        case PalettePolicy::Dark:
            return PaletteRow::Dark;  // Dark overrides environment
    }
    
    // Then, resolve palette row from environment + effective time
    // Crystal's proven mapping from environment_colors.asm:
    //   Outdoor (TOWN, ROUTE): Morn/Day/Nite/Dark → corresponding row directly
    //   Indoor (INDOOR, GATE): Morn/Day → Indoor, Nite → Nite, Dark → Dark
    //   Dungeon (CAVE, DUNGEON): same as Indoor
    switch (env) {
        case Environment::Outdoor:
            // Outdoor: time → corresponding row directly
            return effective_time;
            
        case Environment::Indoor:
        case Environment::Dungeon:
            // Indoor/Dungeon: Morn/Day → Indoor, Nite → Nite, Dark → Dark
            switch (effective_time) {
                case PaletteRow::Morn:
                case PaletteRow::Day:
                    return PaletteRow::Indoor;
                case PaletteRow::Nite:
                    return PaletteRow::Nite;
                case PaletteRow::Dark:
                    return PaletteRow::Dark;
                default:
                    return PaletteRow::Indoor;
            }
    }
    
    return PaletteRow::Day;  // Fallback
}

bool TileRenderer::build_map(VulkanBootstrap& vk, const RuntimeMap& map, const RuntimeTileset& tileset) {
    map_width_ = map.width;
    map_height_ = map.height;
    
    // Store palette_map from tileset for vertex generation
    palette_map_ = tileset.palette_map;
    
    // Map dimensions in tiles (each block is 4×4 tiles)
    const uint32_t tiles_wide = map.width * 4;
    const uint32_t tiles_tall = map.height * 4;
    const uint32_t total_tiles = tiles_wide * tiles_tall;
    
    // Reserve space for all tile instances
    // Each tile is a quad (4 vertices, 6 indices)
    std::vector<TileVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(total_tiles * 4);
    indices.reserve(total_tiles * 6);
    
    const float tile_size = static_cast<float>(config_.tile_size);
    
    // Expand blocks to tile instances
    for (uint8_t by = 0; by < map.height; ++by) {
        for (uint8_t bx = 0; bx < map.width; ++bx) {
            uint8_t block_idx = map.blocks[by * map.width + bx];
            
            // Block 0 is typically "no block" / border
            if (block_idx >= blocks_.size()) {
                block_idx = 0;
            }
            
            const RuntimeBlock& block = blocks_[block_idx];
            
            // Expand this block's 16 tiles (4×4)
            for (int ty = 0; ty < 4; ++ty) {
                for (int tx = 0; tx < 4; ++tx) {
                    uint16_t tile_id = block.tile_ids[ty * 4 + tx];
                    
                    // Clamp to valid tile range
                    if (tile_id >= tile_uvs_.size()) {
                        tile_id = 0;
                    }
                    
                    const TileUV& uv = tile_uvs_[tile_id];
                    
                    // Get palette_id for this tile
                    float pal_id = 0.0f;
                    if (tile_id < palette_map_.size()) {
                        pal_id = static_cast<float>(palette_map_[tile_id]);
                    }
                    
                    // Position in pixels
                    float px = (bx * 4 + tx) * tile_size;
                    float py = (by * 4 + ty) * tile_size;
                    
                    uint32_t base = static_cast<uint32_t>(vertices.size());
                    
                    // Quad vertices (CCW) - include pal_id for future per-instance palette
                    vertices.push_back({px, py, uv.u0, uv.v0, pal_id});                      // TL
                    vertices.push_back({px + tile_size, py, uv.u1, uv.v0, pal_id});          // TR
                    vertices.push_back({px + tile_size, py + tile_size, uv.u1, uv.v1, pal_id}); // BR
                    vertices.push_back({px, py + tile_size, uv.u0, uv.v1, pal_id});          // BL
                    
                    // Two triangles
                    indices.push_back(base + 0);
                    indices.push_back(base + 1);
                    indices.push_back(base + 2);
                    indices.push_back(base + 0);
                    indices.push_back(base + 2);
                    indices.push_back(base + 3);
                }
            }
        }
    }
    
    return create_buffers(vk, vertices, indices);
}

bool TileRenderer::create_buffers(VulkanBootstrap& vk,
                                   const std::vector<TileVertex>& vertices,
                                   const std::vector<uint32_t>& indices) {
    // F4-renderer: Create new buffers into LOCALS.
    // Do NOT call destroy_buffers() first — that would kill the live geometry.
    // Only replace the live buffers after all allocations succeed.
    
    VkDeviceSize vertex_size = vertices.size() * sizeof(TileVertex);
    VkDeviceSize index_size = indices.size() * sizeof(uint32_t);
    
    VkBuffer new_vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_vertex_memory = VK_NULL_HANDLE;
    VkBuffer new_index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_index_memory = VK_NULL_HANDLE;
    
    // Helper: clean up new locals on failure
    auto cleanup_new = [&]() {
        if (new_vertex_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, new_vertex_buffer, nullptr);
            new_vertex_buffer = VK_NULL_HANDLE;
        }
        if (new_vertex_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, new_vertex_memory, nullptr);
            new_vertex_memory = VK_NULL_HANDLE;
        }
        if (new_index_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, new_index_buffer, nullptr);
            new_index_buffer = VK_NULL_HANDLE;
        }
        if (new_index_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, new_index_memory, nullptr);
            new_index_memory = VK_NULL_HANDLE;
        }
    };
    
    // Create vertex buffer
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = vertex_size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(device_, &buffer_info, nullptr, &new_vertex_buffer) != VK_SUCCESS) {
        cleanup_new(); return false;  // live buffers untouched
    }
    
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device_, new_vertex_buffer, &mem_reqs);
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        vk.physical_device(), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &new_vertex_memory) != VK_SUCCESS) {
        cleanup_new(); return false;
    }
    
    vkBindBufferMemory(device_, new_vertex_buffer, new_vertex_memory, 0);
    
    void* data;
    vkMapMemory(device_, new_vertex_memory, 0, vertex_size, 0, &data);
    std::memcpy(data, vertices.data(), vertex_size);
    vkUnmapMemory(device_, new_vertex_memory);
    
    // Create index buffer
    buffer_info.size = index_size;
    buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    
    if (vkCreateBuffer(device_, &buffer_info, nullptr, &new_index_buffer) != VK_SUCCESS) {
        cleanup_new(); return false;
    }
    
    vkGetBufferMemoryRequirements(device_, new_index_buffer, &mem_reqs);
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        vk.physical_device(), mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &new_index_memory) != VK_SUCCESS) {
        cleanup_new(); return false;
    }
    
    vkBindBufferMemory(device_, new_index_buffer, new_index_memory, 0);
    
    vkMapMemory(device_, new_index_memory, 0, index_size, 0, &data);
    std::memcpy(data, indices.data(), index_size);
    vkUnmapMemory(device_, new_index_memory);
    
    // ALL allocations succeeded — destroy old buffers then commit new ones.
    destroy_buffers();
    vertex_buffer_ = new_vertex_buffer;
    vertex_memory_ = new_vertex_memory;
    index_buffer_ = new_index_buffer;
    index_memory_ = new_index_memory;
    index_count_ = static_cast<uint32_t>(indices.size());
    return true;
}

void TileRenderer::set_view(float camera_x, float camera_y) {
    camera_x_ = camera_x;
    camera_y_ = camera_y;
}

void TileRenderer::update_viewport(uint32_t window_width, uint32_t window_height) {
    window_extent_ = {window_width, window_height};
    
    // Calculate largest integer scale that fits
    uint32_t scale_x = window_width / config_.logical_width;
    uint32_t scale_y = window_height / config_.logical_height;
    uint32_t scale = std::min(scale_x, scale_y);
    if (scale < 1) scale = 1;
    
    // Scaled dimensions
    uint32_t scaled_width = config_.logical_width * scale;
    uint32_t scaled_height = config_.logical_height * scale;
    
    // Center in window (letterbox)
    float offset_x = (window_width - scaled_width) / 2.0f;
    float offset_y = (window_height - scaled_height) / 2.0f;
    
    // Set viewport for integer-scaled rendering
    scaled_viewport_.x = offset_x;
    scaled_viewport_.y = offset_y;
    scaled_viewport_.width = static_cast<float>(scaled_width);
    scaled_viewport_.height = static_cast<float>(scaled_height);
    scaled_viewport_.minDepth = 0.0f;
    scaled_viewport_.maxDepth = 1.0f;
    
    // Scissor matches viewport
    scaled_scissor_.offset.x = static_cast<int32_t>(offset_x);
    scaled_scissor_.offset.y = static_cast<int32_t>(offset_y);
    scaled_scissor_.extent.width = scaled_width;
    scaled_scissor_.extent.height = scaled_height;
}

void TileRenderer::render(VkCommandBuffer cmd) {
    if (!pipeline_ || !descriptor_set_ || index_count_ == 0) return;
    
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &descriptor_set_, 0, nullptr);
    
    // Use integer-scaled viewport with letterboxing
    vkCmdSetViewport(cmd, 0, 1, &scaled_viewport_);
    vkCmdSetScissor(cmd, 0, 1, &scaled_scissor_);
    
    // Push constants - use logical resolution for coordinate transform
    float push_data[4] = {camera_x_, camera_y_, 
                          static_cast<float>(config_.logical_width),
                          static_cast<float>(config_.logical_height)};
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push_data), push_data);
    
    // Bind buffers and draw
    VkBuffer buffers[] = {vertex_buffer_};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_, 0, VK_INDEX_TYPE_UINT32);
    
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
}

} // namespace enginemon
