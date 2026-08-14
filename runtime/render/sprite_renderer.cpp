// runtime/render/sprite_renderer.cpp
// Sprite renderer implementation for player and NPCs
//
// Renders sprites on top of the tile map using Vulkan 1.3.
// Uses nearest-neighbor sampling, alpha blending, and position interpolation.

#include "render/sprite_renderer.hpp"
#include "render/vulkan_bootstrap.hpp"
#include <fstream>
#include <cstring>
#include <iostream>
#include <algorithm>

#ifndef SHADER_DIR
#define SHADER_DIR "."
#endif

namespace enginemon {

SpriteRenderer::SpriteRenderer() = default;

SpriteRenderer::~SpriteRenderer() {
    destroy();
}

void SpriteRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(device_);
    
    destroy_buffers();
    atlas_texture_.destroy();
    
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

void SpriteRenderer::destroy_buffers() {
    for (auto& fb : frame_buffers_) {
        if (fb.vertex_buffer) {
            vkDestroyBuffer(device_, fb.vertex_buffer, nullptr);
            fb.vertex_buffer = VK_NULL_HANDLE;
        }
        if (fb.vertex_memory) {
            vkFreeMemory(device_, fb.vertex_memory, nullptr);
            fb.vertex_memory = VK_NULL_HANDLE;
        }
        if (fb.index_buffer) {
            vkDestroyBuffer(device_, fb.index_buffer, nullptr);
            fb.index_buffer = VK_NULL_HANDLE;
        }
        if (fb.index_memory) {
            vkFreeMemory(device_, fb.index_memory, nullptr);
            fb.index_memory = VK_NULL_HANDLE;
        }
        fb.vertex_buffer_size = 0;
        fb.index_buffer_size = 0;
    }
    index_count_ = 0;
}

uint32_t SpriteRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkShaderModule SpriteRenderer::create_shader_module(const uint32_t* code, size_t size) {
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

bool SpriteRenderer::initialize(VulkanBootstrap& vk, const SpriteRendererConfig& config) {
    device_ = vk.device();
    physical_device_ = vk.physical_device();
    config_ = config;
    
    return create_pipeline(vk);
}

bool SpriteRenderer::create_pipeline(VulkanBootstrap& vk) {
    // Load shader files (reuse tile shaders - they work for sprites too)
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
    
    // Vertex input: position (vec2) + UV (vec2)
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(SpriteVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(SpriteVertex, x);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(SpriteVertex, u);
    
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
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
    
    // Color blending (alpha blend for transparency)
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

    // Descriptor set layout (for sprite atlas texture)
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
        std::cerr << "Failed to create sprite graphics pipeline\n";
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

bool SpriteRenderer::set_atlas(VulkanBootstrap& vk, const RuntimeSpriteAtlas& atlas) {
    // Upload texture
    if (!atlas_texture_.create(vk, atlas.atlas_width, atlas.atlas_height, atlas.pixels.data())) {
        return false;
    }
    
    // Build UV lookup
    sprite_uvs_.clear();
    for (const auto& uvs : atlas.sprite_uvs) {
        sprite_uvs_[uvs.sprite_id] = uvs;
    }
    
    // Reset descriptor pool before allocating new set
    // This frees any previously allocated descriptor sets
    vkResetDescriptorPool(device_, descriptor_pool_, 0);
    
    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_layout_;
    
    if (vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) != VK_SUCCESS) {
        return false;
    }
    
    // Update descriptor
    VkDescriptorImageInfo image_info{};
    image_info.sampler = atlas_texture_.sampler();
    image_info.imageView = atlas_texture_.view();
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    
    return true;
}

void SpriteRenderer::set_sprite_data(const std::vector<RuntimeSprite>& sprites) {
    sprites_data_.clear();
    for (const auto& sprite : sprites) {
        sprites_data_[sprite.sprite_id] = sprite;
    }
}

void SpriteRenderer::set_view(float camera_x, float camera_y) {
    camera_x_ = camera_x;
    camera_y_ = camera_y;
}

void SpriteRenderer::update_viewport(uint32_t window_width, uint32_t window_height) {
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

void SpriteRenderer::set_sprites(const std::vector<SpriteInstance>& sprites) {
    sprites_ = sprites;
}

bool SpriteRenderer::prepare_buffers(VulkanBootstrap& vk, size_t max_sprites) {
    // Each sprite = 4 vertices + 6 indices
    // Allocate for all frames in flight
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (!ensure_buffers(vk, max_sprites * 4, max_sprites * 6, i)) {
            return false;
        }
    }
    return true;
}

bool SpriteRenderer::ensure_buffers(VulkanBootstrap& vk, size_t vertex_count, size_t index_count, uint32_t frame_index) {
    auto& fb = frame_buffers_[frame_index];
    
    size_t vertex_size = vertex_count * sizeof(SpriteVertex);
    size_t index_size = index_count * sizeof(uint16_t);
    
    // Only reallocate if needed
    if (vertex_size <= fb.vertex_buffer_size && index_size <= fb.index_buffer_size) {
        return true;
    }
    
    // Destroy old buffers for this frame
    if (fb.vertex_buffer) {
        vkDestroyBuffer(device_, fb.vertex_buffer, nullptr);
        fb.vertex_buffer = VK_NULL_HANDLE;
    }
    if (fb.vertex_memory) {
        vkFreeMemory(device_, fb.vertex_memory, nullptr);
        fb.vertex_memory = VK_NULL_HANDLE;
    }
    if (fb.index_buffer) {
        vkDestroyBuffer(device_, fb.index_buffer, nullptr);
        fb.index_buffer = VK_NULL_HANDLE;
    }
    if (fb.index_memory) {
        vkFreeMemory(device_, fb.index_memory, nullptr);
        fb.index_memory = VK_NULL_HANDLE;
    }
    
    // Allocate with some headroom
    fb.vertex_buffer_size = vertex_size * 2;
    fb.index_buffer_size = index_size * 2;
    
    // Create vertex buffer
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = fb.vertex_buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(device_, &buffer_info, nullptr, &fb.vertex_buffer) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device_, fb.vertex_buffer, &mem_reqs);
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &fb.vertex_memory) != VK_SUCCESS) {
        return false;
    }
    
    vkBindBufferMemory(device_, fb.vertex_buffer, fb.vertex_memory, 0);
    
    // Create index buffer
    buffer_info.size = fb.index_buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    
    if (vkCreateBuffer(device_, &buffer_info, nullptr, &fb.index_buffer) != VK_SUCCESS) {
        return false;
    }
    
    vkGetBufferMemoryRequirements(device_, fb.index_buffer, &mem_reqs);
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &fb.index_memory) != VK_SUCCESS) {
        return false;
    }
    
    vkBindBufferMemory(device_, fb.index_buffer, fb.index_memory, 0);
    
    return true;
}

void SpriteRenderer::build_geometry(std::vector<SpriteVertex>& vertices, 
                                     std::vector<uint16_t>& indices) {
    vertices.clear();
    indices.clear();
    
    if (sprites_.empty()) return;
    
    vertices.reserve(sprites_.size() * 4);
    indices.reserve(sprites_.size() * 6);
    
    for (const auto& sprite : sprites_) {
        // Find sprite UVs
        auto uv_it = sprite_uvs_.find(sprite.sprite_id);
        if (uv_it == sprite_uvs_.end()) {
            continue;  // Unknown sprite
        }
        
        const auto& uvs = uv_it->second;
        
        // Find sprite data for semantic frame selection
        auto data_it = sprites_data_.find(sprite.sprite_id);
        
        int frame_idx = 0;
        bool flip_x = false;
        
        if (data_it != sprites_data_.end()) {
            // Use semantic frame selection from RuntimeSprite
            auto selection = data_it->second.get_frame(
                sprite.facing, 
                sprite.walking, 
                sprite.step_flip
            );
            frame_idx = selection.frame_index;
            flip_x = selection.flip_x;
        }
        
        if (frame_idx >= uvs.frame_count) {
            frame_idx = 0;  // Fallback to first frame
        }
        
        // Get UV for this frame
        const auto& frame_uv = uvs.frame_uvs[frame_idx];
        float u0 = frame_uv[0];
        float v0 = frame_uv[1];
        float u1 = frame_uv[2];
        float v1 = frame_uv[3];
        
        // Handle horizontal flip for right-facing
        if (flip_x) {
            std::swap(u0, u1);
        }
        
        // Calculate render position with interpolation
        float px = sprite.render_x();
        float py = sprite.render_y();
        
        // Sprites are 16×16 pixels
        constexpr float SPRITE_SIZE = 16.0f;
        
        uint16_t base = static_cast<uint16_t>(vertices.size());
        
        // Quad vertices (CCW)
        vertices.push_back({px, py, u0, v0});                           // TL
        vertices.push_back({px + SPRITE_SIZE, py, u1, v0});             // TR
        vertices.push_back({px + SPRITE_SIZE, py + SPRITE_SIZE, u1, v1}); // BR
        vertices.push_back({px, py + SPRITE_SIZE, u0, v1});             // BL
        
        // Two triangles
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

void SpriteRenderer::render(VkCommandBuffer cmd) {
    if (!pipeline_ || !descriptor_set_ || sprites_.empty()) return;
    
    // Build geometry for current sprites
    std::vector<SpriteVertex> vertices;
    std::vector<uint16_t> indices;
    build_geometry(vertices, indices);
    
    if (indices.empty()) return;
    
    // Use per-frame buffers to avoid data races with GPU
    // current_frame_ cycles 0, 1, 0, 1, ... matching VulkanBootstrap::current_frame_
    auto& fb = frame_buffers_[current_frame_];
    
    // For simplicity, we'll upload directly (this requires host-visible memory)
    if (!fb.vertex_buffer || !fb.index_buffer) return;
    
    // Upload vertex data
    void* data;
    VkDeviceSize vertex_size = vertices.size() * sizeof(SpriteVertex);
    VkDeviceSize index_size = indices.size() * sizeof(uint16_t);
    
    if (vertex_size > fb.vertex_buffer_size || index_size > fb.index_buffer_size) {
        // Can't reallocate during render - skip this frame
        return;
    }
    
    vkMapMemory(device_, fb.vertex_memory, 0, vertex_size, 0, &data);
    std::memcpy(data, vertices.data(), vertex_size);
    vkUnmapMemory(device_, fb.vertex_memory);
    
    vkMapMemory(device_, fb.index_memory, 0, index_size, 0, &data);
    std::memcpy(data, indices.data(), index_size);
    vkUnmapMemory(device_, fb.index_memory);
    
    index_count_ = static_cast<uint32_t>(indices.size());
    
    // Render
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &descriptor_set_, 0, nullptr);
    
    // Use integer-scaled viewport with letterboxing
    vkCmdSetViewport(cmd, 0, 1, &scaled_viewport_);
    vkCmdSetScissor(cmd, 0, 1, &scaled_scissor_);
    
    // Push constants
    float push_data[4] = {camera_x_, camera_y_, 
                          static_cast<float>(config_.logical_width),
                          static_cast<float>(config_.logical_height)};
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push_data), push_data);
    
    // Bind buffers and draw
    VkBuffer buffers[] = {fb.vertex_buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, fb.index_buffer, 0, VK_INDEX_TYPE_UINT16);
    
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
    
    // Advance to next frame's buffers for next render call
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

} // namespace enginemon
