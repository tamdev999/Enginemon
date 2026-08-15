// runtime/render/textbox_renderer.cpp
// Native textbox renderer implementation
//
// Uses compiled FontDefinition from EMON package for rendering.
// Supports multi-page text flow with semantic control operations.
//
// NOTE: This file contains ZERO Crystal-specific encoding knowledge.
// Text arrives as semantic NativeTextSequence with UTF-8 strings.

#include "render/textbox_renderer.hpp"
#include "render/vulkan_bootstrap.hpp"
#include "engine/scripting/api_bindings.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>

#ifndef SHADER_DIR
#define SHADER_DIR "."
#endif

namespace enginemon {

//=============================================================================
// Helper to extract a single UTF-8 character from a string
// Returns the character and advances the pointer
//=============================================================================
static std::string extract_utf8_char(const char*& p, const char* end) {
    if (p >= end) return "";
    
    unsigned char ch = static_cast<unsigned char>(*p);
    size_t len = 1;
    
    // Determine UTF-8 sequence length
    if ((ch & 0x80) == 0) {
        len = 1;  // ASCII
    } else if ((ch & 0xE0) == 0xC0) {
        len = 2;  // 2-byte sequence
    } else if ((ch & 0xF0) == 0xE0) {
        len = 3;  // 3-byte sequence
    } else if ((ch & 0xF8) == 0xF0) {
        len = 4;  // 4-byte sequence
    }
    
    // Check we have enough bytes
    if (p + len > end) {
        len = 1;  // Truncated, just take one byte
    }
    
    std::string result(p, len);
    p += len;
    return result;
}


//=============================================================================
// NativeTextSequence::from_runtime
// Converts RuntimeTextSequence to NativeTextSequence
//=============================================================================
NativeTextSequence NativeTextSequence::from_runtime(const RuntimeTextSequence& seq) {
    NativeTextSequence result;
    for (const auto& elem : seq.elements) {
        NativeTextElement native;
        switch (elem.op) {
            case RuntimeTextOp::Text:
                native.control = TextControl::None;
                native.text = elem.text;
                break;
            case RuntimeTextOp::Line:
                native.control = TextControl::Line;
                break;
            case RuntimeTextOp::Next:
                native.control = TextControl::Next;
                break;
            case RuntimeTextOp::Para:
                native.control = TextControl::Para;
                break;
            case RuntimeTextOp::Cont:
            case RuntimeTextOp::Scroll:
                native.control = TextControl::Cont;
                break;
            case RuntimeTextOp::Done:
                native.control = TextControl::Done;
                break;
            case RuntimeTextOp::Prompt:
                native.control = TextControl::Prompt;
                break;
        }
        result.elements.push_back(native);
    }
    return result;
}

//=============================================================================
// RuntimeFontAtlas::from_package_data
// Parses serialized font atlas data from the EMON package (v2 format)
// Builds UTF-8 → GlyphId lookup (no Crystal charmap in runtime)
//
// v2 format removes crystal_code - font entries are now:
//   glyph_index: u16
//   is_control: u8
//   control_name_len: u16, control_name: bytes
//   utf8_len: u16, utf8_char: bytes
//=============================================================================
template<typename T>
static T read_le(const uint8_t*& ptr) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(*ptr++) << (i * 8);
    }
    return value;
}

static float read_float_le(const uint8_t*& ptr) {
    uint32_t bits = read_le<uint32_t>(ptr);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}


bool RuntimeFontAtlas::from_package_data(const std::vector<uint8_t>& data, RuntimeFontAtlas& atlas) {
    if (data.size() < 16) return false;
    
    const uint8_t* ptr = data.data();
    const uint8_t* end = data.data() + data.size();
    
    // Read header
    atlas.atlas_width = read_le<uint32_t>(ptr);
    atlas.atlas_height = read_le<uint32_t>(ptr);
    
    // Read pixels
    uint32_t pixel_count = read_le<uint32_t>(ptr);
    if (ptr + pixel_count * 4 > end) return false;
    
    atlas.pixels.resize(pixel_count);
    for (uint32_t i = 0; i < pixel_count; ++i) {
        atlas.pixels[i] = read_le<uint32_t>(ptr);
    }
    
    // Read glyph UVs
    if (ptr + 4 > end) return false;
    uint32_t uv_count = read_le<uint32_t>(ptr);
    if (ptr + uv_count * 16 > end) return false;
    
    atlas.glyph_uvs.resize(uv_count);
    for (uint32_t i = 0; i < uv_count; ++i) {
        atlas.glyph_uvs[i].u0 = read_float_le(ptr);
        atlas.glyph_uvs[i].v0 = read_float_le(ptr);
        atlas.glyph_uvs[i].u1 = read_float_le(ptr);
        atlas.glyph_uvs[i].v1 = read_float_le(ptr);
    }
    
    // Read charmap and build UTF-8 → GlyphId lookup (v2 format: no crystal_code)
    if (ptr + 4 > end) return false;
    uint32_t charmap_count = read_le<uint32_t>(ptr);
    
    atlas.utf8_to_glyph.clear();
    
    for (uint32_t i = 0; i < charmap_count; ++i) {
        if (ptr + 3 > end) return false;
        
        // v2 format: glyph_index, is_control, control_name, utf8_char (no crystal_code)
        uint16_t glyph_index = read_le<uint16_t>(ptr);
        uint8_t is_control = *ptr++;
        
        // Skip control_name
        if (ptr + 2 > end) return false;
        uint16_t control_name_len = read_le<uint16_t>(ptr);
        if (ptr + control_name_len > end) return false;
        ptr += control_name_len;
        
        // Read utf8_char - THIS is what we use for lookup
        if (ptr + 2 > end) return false;
        uint16_t utf8_len = read_le<uint16_t>(ptr);
        if (ptr + utf8_len > end) return false;
        
        if (!is_control && utf8_len > 0) {
            std::string utf8_char(reinterpret_cast<const char*>(ptr), utf8_len);
            atlas.utf8_to_glyph[utf8_char] = glyph_index;
        }
        ptr += utf8_len;
    }
    
    // Read special glyph indices
    if (ptr + 16 > end) return false;
    atlas.border_tl = read_le<uint16_t>(ptr);
    atlas.border_t = read_le<uint16_t>(ptr);
    atlas.border_tr = read_le<uint16_t>(ptr);
    atlas.border_l = read_le<uint16_t>(ptr);
    atlas.border_bl = read_le<uint16_t>(ptr);
    atlas.border_br = read_le<uint16_t>(ptr);
    atlas.space_glyph = read_le<uint16_t>(ptr);
    atlas.cursor_glyph = read_le<uint16_t>(ptr);
    
    // Ensure space is in the lookup
    atlas.utf8_to_glyph[" "] = atlas.space_glyph;
    
    atlas.font_id = "main_font";
    return true;
}


TextboxRenderer::TextboxRenderer() = default;
TextboxRenderer::~TextboxRenderer() { destroy(); }

static std::vector<uint32_t> load_shader_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Failed to open shader: " << path << "\n";
        return {};
    }
    size_t size = file.tellg();
    file.seekg(0);
    std::vector<uint32_t> code(size / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(code.data()), size);
    return code;
}

bool TextboxRenderer::initialize(VulkanBootstrap& vk, const TextboxRendererConfig& config) {
    device_ = vk.device();
    physical_device_ = vk.physical_device();
    config_ = config;
    
    if (!create_pipeline(vk)) return false;
    if (!ensure_buffers(vk, 2048, 3072)) return false;
    
    update_viewport(vk.swapchain_extent().width, vk.swapchain_extent().height);
    return true;
}

bool TextboxRenderer::load_font_atlas(VulkanBootstrap& vk, const RuntimeFontAtlas& atlas) {
    font_atlas_ = atlas;
    has_font_ = true;
    return create_font_texture(vk);
}

void TextboxRenderer::update_viewport(uint32_t window_width, uint32_t window_height) {
    float scale_x = static_cast<float>(window_width) / config_.logical_width;
    float scale_y = static_cast<float>(window_height) / config_.logical_height;
    float scale = std::floor(std::min(scale_x, scale_y));
    if (scale < 1.0f) scale = 1.0f;
    
    float vp_width = config_.logical_width * scale;
    float vp_height = config_.logical_height * scale;
    float vp_x = (window_width - vp_width) / 2.0f;
    float vp_y = (window_height - vp_height) / 2.0f;
    
    scaled_viewport_ = {vp_x, vp_y, vp_width, vp_height, 0.0f, 1.0f};
    scaled_scissor_.offset = {static_cast<int32_t>(vp_x), static_cast<int32_t>(vp_y)};
    scaled_scissor_.extent = {static_cast<uint32_t>(vp_width), static_cast<uint32_t>(vp_height)};
}

void TextboxRenderer::set_state(const TextboxState& state) { 
    state_ = state; 
}

uint32_t TextboxRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && 
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0;
}


VkShaderModule TextboxRenderer::create_shader_module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = code;
    VkShaderModule module;
    if (vkCreateShaderModule(device_, &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

bool TextboxRenderer::create_font_texture(VulkanBootstrap& vk) {
    if (!has_font_ || font_atlas_.pixels.empty()) {
        std::cerr << "No font atlas loaded\n";
        return false;
    }
    
    VkDeviceSize image_size = font_atlas_.pixels.size() * sizeof(uint32_t);
    
    // Create staging buffer
    VkBufferCreateInfo staging_buf_info{};
    staging_buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_buf_info.size = image_size;
    staging_buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    if (vkCreateBuffer(device_, &staging_buf_info, nullptr, &staging_buffer) != VK_SUCCESS) return false;
    
    VkMemoryRequirements staging_reqs;
    vkGetBufferMemoryRequirements(device_, staging_buffer, &staging_reqs);
    
    VkMemoryAllocateInfo staging_alloc{};
    staging_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    staging_alloc.allocationSize = staging_reqs.size;
    staging_alloc.memoryTypeIndex = find_memory_type(staging_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(device_, &staging_alloc, nullptr, &staging_memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging_buffer, nullptr);
        return false;
    }
    vkBindBufferMemory(device_, staging_buffer, staging_memory, 0);
    
    void* data;
    vkMapMemory(device_, staging_memory, 0, image_size, 0, &data);
    memcpy(data, font_atlas_.pixels.data(), image_size);
    vkUnmapMemory(device_, staging_memory);

    // Create image
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent = {font_atlas_.atlas_width, font_atlas_.atlas_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    
    if (vkCreateImage(device_, &image_info, nullptr, &font_image_) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging_buffer, nullptr);
        vkFreeMemory(device_, staging_memory, nullptr);
        return false;
    }
    
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, font_image_, &mem_reqs);
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, 
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &font_memory_) != VK_SUCCESS) {
        vkDestroyImage(device_, font_image_, nullptr);
        vkDestroyBuffer(device_, staging_buffer, nullptr);
        vkFreeMemory(device_, staging_memory, nullptr);
        return false;
    }
    vkBindImageMemory(device_, font_image_, font_memory_, 0);

    // Transition and copy
    VkCommandBuffer cmd = vk.get_command_buffer();
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin_info);
    
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = font_image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {font_atlas_.atlas_width, font_atlas_.atlas_height, 1};
    
    vkCmdCopyBufferToImage(cmd, staging_buffer, font_image_, 
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, 
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    
    vkQueueSubmit(vk.graphics_queue(), 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.graphics_queue());
    
    vkDestroyBuffer(device_, staging_buffer, nullptr);
    vkFreeMemory(device_, staging_memory, nullptr);
    
    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = font_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view_info, nullptr, &font_view_) != VK_SUCCESS) return false;

    // Create sampler (NEAREST for pixel-perfect)
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &font_sampler_) != VK_SUCCESS) return false;
    
    // Update descriptor set
    VkDescriptorImageInfo img_info{font_sampler_, font_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    
    return true;
}

bool TextboxRenderer::create_pipeline(VulkanBootstrap& vk) {
    // Descriptor set layout
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_layout_) != VK_SUCCESS)
        return false;
    
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) 
        return false;
    
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) != VK_SUCCESS) 
        return false;
    
    VkPipelineLayoutCreateInfo pl_info{};
    pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount = 1;
    pl_info.pSetLayouts = &descriptor_layout_;
    if (vkCreatePipelineLayout(device_, &pl_info, nullptr, &pipeline_layout_) != VK_SUCCESS) 
        return false;

    // Load shaders
    std::string shader_dir = SHADER_DIR;
    auto vert_code = load_shader_file(shader_dir + "/textbox.vert.spv");
    auto frag_code = load_shader_file(shader_dir + "/textbox.frag.spv");
    if (vert_code.empty() || frag_code.empty()) {
        std::cerr << "Failed to load textbox shaders from " << shader_dir << "\n";
        return false;
    }
    
    VkShaderModule vert_module = create_shader_module(vert_code.data(), vert_code.size() * sizeof(uint32_t));
    VkShaderModule frag_module = create_shader_module(frag_code.data(), frag_code.size() * sizeof(uint32_t));
    if (!vert_module || !frag_module) {
        if (vert_module) vkDestroyShaderModule(device_, vert_module, nullptr);
        if (frag_module) vkDestroyShaderModule(device_, frag_module, nullptr);
        return false;
    }
    
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vb{0, sizeof(TextboxVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextboxVertex, x)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextboxVertex, u)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TextboxVertex, r)};
    
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vb;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attach{};
    blend_attach.blendEnable = VK_TRUE;
    blend_attach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attach.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attach.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attach;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo render_info{};
    render_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render_info.colorAttachmentCount = 1;
    VkFormat format = vk.swapchain_format();
    render_info.pColorAttachmentFormats = &format;
    
    VkGraphicsPipelineCreateInfo pipe_info{};
    pipe_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe_info.pNext = &render_info;
    pipe_info.stageCount = 2;
    pipe_info.pStages = stages;
    pipe_info.pVertexInputState = &vertex_input;
    pipe_info.pInputAssemblyState = &input_assembly;
    pipe_info.pViewportState = &viewport_state;
    pipe_info.pRasterizationState = &raster;
    pipe_info.pMultisampleState = &multisample;
    pipe_info.pColorBlendState = &blend;
    pipe_info.pDynamicState = &dynamic;
    pipe_info.layout = pipeline_layout_;
    
    VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipe_info, 
        nullptr, &pipeline_);
    
    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);
    
    return result == VK_SUCCESS;
}


bool TextboxRenderer::ensure_buffers(VulkanBootstrap& vk, size_t vertex_count, size_t index_count) {
    size_t vb_size = vertex_count * sizeof(TextboxVertex);
    size_t ib_size = index_count * sizeof(uint16_t);
    
    if (vb_size > vertex_buffer_size_) {
        if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
        if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);
        
        VkBufferCreateInfo buf_info{};
        buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_info.size = vb_size;
        buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        vkCreateBuffer(device_, &buf_info, nullptr, &vertex_buffer_);
        
        VkMemoryRequirements reqs;
        vkGetBufferMemoryRequirements(device_, vertex_buffer_, &reqs);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = find_memory_type(reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &alloc, nullptr, &vertex_memory_);
        vkBindBufferMemory(device_, vertex_buffer_, vertex_memory_, 0);
        vertex_buffer_size_ = vb_size;
    }
    
    if (ib_size > index_buffer_size_) {
        if (index_buffer_) vkDestroyBuffer(device_, index_buffer_, nullptr);
        if (index_memory_) vkFreeMemory(device_, index_memory_, nullptr);
        
        VkBufferCreateInfo buf_info{};
        buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_info.size = ib_size;
        buf_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        vkCreateBuffer(device_, &buf_info, nullptr, &index_buffer_);
        
        VkMemoryRequirements reqs;
        vkGetBufferMemoryRequirements(device_, index_buffer_, &reqs);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = find_memory_type(reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &alloc, nullptr, &index_memory_);
        vkBindBufferMemory(device_, index_buffer_, index_memory_, 0);
        index_buffer_size_ = ib_size;
    }
    return true;
}


//=============================================================================
// parse_text_pages - Parse NativeTextSequence into displayable pages
// Uses semantic TextControl operations (no Crystal byte codes)
//=============================================================================
void TextboxRenderer::parse_text_pages(TextboxState& state) {
    state.pages.clear();
    state.page_meta.clear();
    state.visible.clear();
    
    if (state.text_sequence.empty()) return;
    
    const size_t max_chars_per_line = config_.text_width_tiles;
    const size_t max_lines = config_.max_lines;
    
    TextPage current_page;
    std::string current_line;
    PageMeta current_meta;
    current_meta.stream_start = 0;
    
    for (size_t i = 0; i < state.text_sequence.elements.size(); ++i) {
        const auto& elem = state.text_sequence.elements[i];
        
        if (elem.is_text()) {
            // Process text character by character
            const char* p = elem.text.c_str();
            const char* end = p + elem.text.size();
            
            while (p < end) {
                std::string ch = extract_utf8_char(p, end);
                if (ch.empty()) continue;
                
                current_line += ch;
                
                // Check for line wrap
                // Count UTF-8 characters in line (not bytes)
                size_t char_count = 0;
                const char* lp = current_line.c_str();
                const char* lend = lp + current_line.size();
                while (lp < lend) {
                    extract_utf8_char(lp, lend);
                    char_count++;
                }
                
                if (char_count >= max_chars_per_line) {
                    current_page.lines.push_back(current_line);
                    current_line.clear();
                    
                    size_t page_max_lines = current_page.is_cont_page ? 1 : max_lines;
                    if (current_page.lines.size() >= page_max_lines) {
                        current_meta.stream_end = i + 1;
                        current_meta.ends_with_cont = true;
                        state.pages.push_back(current_page);
                        state.page_meta.push_back(current_meta);
                        
                        current_page = TextPage{};
                        current_page.is_cont_page = true;
                        current_meta = PageMeta{};
                        current_meta.stream_start = i + 1;
                    }
                }
            }
        }
        else {
            // Handle control operation
            switch (elem.control) {
                case TextControl::Done:
                case TextControl::Terminator:
                    if (!current_line.empty()) {
                        current_page.lines.push_back(current_line);
                    }
                    if (!current_page.lines.empty()) {
                        current_meta.stream_end = i;
                        current_meta.is_final = true;
                        state.pages.push_back(current_page);
                        state.page_meta.push_back(current_meta);
                    }
                    goto done_parsing;

                case TextControl::Prompt:
                    if (!current_line.empty()) {
                        current_page.lines.push_back(current_line);
                    }
                    if (!current_page.lines.empty()) {
                        current_meta.stream_end = i;
                        current_meta.is_final = true;
                        state.pages.push_back(current_page);
                        state.page_meta.push_back(current_meta);
                    }
                    goto done_parsing;
                    
                case TextControl::Line:
                    current_page.lines.push_back(current_line);
                    current_line.clear();
                    {
                        size_t page_max_lines = current_page.is_cont_page ? 1 : max_lines;
                        if (current_page.lines.size() >= page_max_lines) {
                            current_meta.stream_end = i + 1;
                            current_meta.ends_with_cont = true;
                            state.pages.push_back(current_page);
                            state.page_meta.push_back(current_meta);
                            
                            current_page = TextPage{};
                            current_page.is_cont_page = true;
                            current_meta = PageMeta{};
                            current_meta.stream_start = i + 1;
                        }
                    }
                    break;
                    
                case TextControl::Next:
                    current_page.lines.push_back(current_line);
                    current_line.clear();
                    break;
                    
                case TextControl::Para:
                    if (!current_line.empty()) {
                        current_page.lines.push_back(current_line);
                    }
                    if (!current_page.lines.empty()) {
                        current_meta.stream_end = i + 1;
                        current_meta.ends_with_para = true;
                        state.pages.push_back(current_page);
                        state.page_meta.push_back(current_meta);
                    }
                    current_page = TextPage{};
                    current_page.is_cont_page = false;
                    current_meta = PageMeta{};
                    current_meta.stream_start = i + 1;
                    current_line.clear();
                    break;
                    
                case TextControl::Cont:
                    if (!current_line.empty()) {
                        current_page.lines.push_back(current_line);
                    }
                    if (!current_page.lines.empty()) {
                        current_meta.stream_end = i + 1;
                        current_meta.ends_with_cont = true;
                        state.pages.push_back(current_page);
                        state.page_meta.push_back(current_meta);
                    }
                    current_page = TextPage{};
                    current_page.is_cont_page = true;
                    current_meta = PageMeta{};
                    current_meta.stream_start = i + 1;
                    current_line.clear();
                    break;
                    
                default:
                    break;
            }
        }
    }

    // Handle remaining text without terminator
    if (!current_line.empty()) {
        current_page.lines.push_back(current_line);
    }
    if (!current_page.lines.empty()) {
        current_meta.stream_end = state.text_sequence.elements.size();
        current_meta.is_final = true;
        state.pages.push_back(current_page);
        state.page_meta.push_back(current_meta);
    }

done_parsing:
    // Initialize visible buffer with first page
    if (!state.pages.empty()) {
        const auto& first_page = state.pages[0];
        if (first_page.lines.size() > 0) {
            state.visible.line1 = first_page.lines[0];
        }
        if (first_page.lines.size() > 1) {
            state.visible.line2 = first_page.lines[1];
        }
    }
    
    // Set initial wait state
    if (!state.pages.empty() && !state.page_meta.empty()) {
        const auto& meta = state.page_meta[0];
        state.waiting_for_para = meta.ends_with_para;
        state.waiting_for_cont = meta.ends_with_cont;
        state.text_complete = meta.is_final && state.page_meta.size() == 1;
    }
}

//=============================================================================
// TextboxState::open_with_sequence
// Opens textbox with semantic text sequence (no Crystal encoding)
//=============================================================================
void TextboxState::open_with_sequence(const RuntimeTextSequence& seq) {
    is_open = true;
    waiting_for_input = true;
    show_cursor = true;
    waiting_for_para = false;
    waiting_for_cont = false;
    text_complete = false;
    chars_revealed = 0;
    current_page = 0;
    pages.clear();
    page_meta.clear();
    visible.clear();
    
    // Convert to native representation
    text_sequence = NativeTextSequence::from_runtime(seq);
    
    // Ensure we have a terminator
    if (text_sequence.elements.empty() || 
        (text_sequence.elements.back().control != TextControl::Done &&
         text_sequence.elements.back().control != TextControl::Prompt)) {
        text_sequence.elements.push_back(NativeTextElement{TextControl::Done, ""});
    }
    
    // Pages will be built by TextboxRenderer::parse_text_pages()
}


//=============================================================================
// Geometry building helpers
//=============================================================================

void TextboxRenderer::add_glyph_quad(std::vector<TextboxVertex>& vertices, 
    std::vector<uint16_t>& indices, float px, float py, uint16_t glyph_index,
    float r, float g, float b, float a) {
    
    if (glyph_index >= font_atlas_.glyph_uvs.size()) return;
    
    const auto& uv = font_atlas_.glyph_uvs[glyph_index];
    float w = static_cast<float>(config_.logical_width);
    float h = static_cast<float>(config_.logical_height);
    float tile = static_cast<float>(config_.tile_size);
    
    float x0 = (px / w) * 2.0f - 1.0f;
    float y0 = (py / h) * 2.0f - 1.0f;
    float x1 = ((px + tile) / w) * 2.0f - 1.0f;
    float y1 = ((py + tile) / h) * 2.0f - 1.0f;
    
    uint16_t base = static_cast<uint16_t>(vertices.size());
    vertices.push_back({x0, y0, uv.u0, uv.v0, r, g, b, a});
    vertices.push_back({x1, y0, uv.u1, uv.v0, r, g, b, a});
    vertices.push_back({x1, y1, uv.u1, uv.v1, r, g, b, a});
    vertices.push_back({x0, y1, uv.u0, uv.v1, r, g, b, a});
    
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

void TextboxRenderer::add_solid_quad(std::vector<TextboxVertex>& vertices, 
    std::vector<uint16_t>& indices, float px, float py, float width, float height,
    float r, float g, float b, float a) {
    
    float w = static_cast<float>(config_.logical_width);
    float h = static_cast<float>(config_.logical_height);
    
    float x0 = (px / w) * 2.0f - 1.0f;
    float y0 = (py / h) * 2.0f - 1.0f;
    float x1 = ((px + width) / w) * 2.0f - 1.0f;
    float y1 = ((py + height) / h) * 2.0f - 1.0f;
    
    const auto& uv = font_atlas_.glyph_uvs[font_atlas_.space_glyph];
    
    uint16_t base = static_cast<uint16_t>(vertices.size());
    vertices.push_back({x0, y0, uv.u0, uv.v0, r, g, b, a});
    vertices.push_back({x1, y0, uv.u1, uv.v0, r, g, b, a});
    vertices.push_back({x1, y1, uv.u1, uv.v1, r, g, b, a});
    vertices.push_back({x0, y1, uv.u0, uv.v1, r, g, b, a});
    
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}


void TextboxRenderer::add_border(std::vector<TextboxVertex>& vertices, 
    std::vector<uint16_t>& indices) {
    
    float tile = static_cast<float>(config_.tile_size);
    float bx = static_cast<float>(config_.box_tile_x) * tile;
    float by = static_cast<float>(config_.box_tile_y) * tile;
    float box_width = static_cast<float>(config_.box_width_tiles) * tile;
    float box_height = (config_.box_inner_height + 2) * tile;
    
    // Opaque white background (vertex alpha=1.0 triggers solid mode)
    add_solid_quad(vertices, indices, bx, by, box_width, box_height,
                   1.0f, 1.0f, 1.0f, 1.0f);
    
    // Border glyphs (vertex alpha=0.0 triggers glyph mode)
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    
    // Top row
    add_glyph_quad(vertices, indices, bx, by, font_atlas_.border_tl, r, g, b, a);
    for (uint32_t x = 1; x < config_.box_width_tiles - 1; ++x) {
        add_glyph_quad(vertices, indices, bx + x * tile, by, font_atlas_.border_t, r, g, b, a);
    }
    add_glyph_quad(vertices, indices, bx + box_width - tile, by, font_atlas_.border_tr, r, g, b, a);

    // Middle rows
    for (uint32_t row = 1; row <= config_.box_inner_height; ++row) {
        float ry = by + row * tile;
        add_glyph_quad(vertices, indices, bx, ry, font_atlas_.border_l, r, g, b, a);
        add_glyph_quad(vertices, indices, bx + box_width - tile, ry, font_atlas_.border_l, r, g, b, a);
    }
    
    // Bottom row
    float bottom_y = by + (config_.box_inner_height + 1) * tile;
    add_glyph_quad(vertices, indices, bx, bottom_y, font_atlas_.border_bl, r, g, b, a);
    for (uint32_t x = 1; x < config_.box_width_tiles - 1; ++x) {
        add_glyph_quad(vertices, indices, bx + x * tile, bottom_y, font_atlas_.border_t, r, g, b, a);
    }
    add_glyph_quad(vertices, indices, bx + box_width - tile, bottom_y, font_atlas_.border_br, r, g, b, a);
}


//=============================================================================
// add_text - Renders visible text using UTF-8 → GlyphId lookup
// NO Crystal encoding knowledge - uses font atlas utf8_to_glyph map
//=============================================================================
void TextboxRenderer::add_text(std::vector<TextboxVertex>& vertices, 
    std::vector<uint16_t>& indices) {
    
    float tile = static_cast<float>(config_.tile_size);
    float text_x = static_cast<float>(config_.text_start_tile_x) * tile;
    float text_y = static_cast<float>(config_.text_start_tile_y) * tile;
    
    // Glyph mode: vertex alpha=0.0
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    
    // Render from visible buffer (UTF-8 strings)
    auto render_line = [&](const std::string& line, float y) {
        float x = text_x;
        const char* p = line.c_str();
        const char* end = p + line.size();
        
        while (p < end) {
            std::string ch = extract_utf8_char(p, end);
            if (ch.empty()) continue;
            
            uint16_t glyph = font_atlas_.glyph_for_utf8(ch);
            add_glyph_quad(vertices, indices, x, y, glyph, r, g, b, a);
            x += tile;
            
            if (x >= text_x + config_.text_width_tiles * tile) break;
        }
    };
    
    if (!state_.visible.line1.empty()) {
        render_line(state_.visible.line1, text_y);
    }
    if (!state_.visible.line2.empty()) {
        render_line(state_.visible.line2, text_y + tile);
    }
    
    // Show cursor if waiting for input
    if (state_.show_cursor || state_.waiting_for_input) {
        float cursor_x = (config_.box_width_tiles - 2) * tile;
        float cursor_y = (config_.box_tile_y + config_.box_inner_height + 1) * tile - tile / 2;
        add_glyph_quad(vertices, indices, cursor_x, cursor_y, font_atlas_.cursor_glyph, r, g, b, a);
    }
}

void TextboxRenderer::build_geometry(std::vector<TextboxVertex>& vertices, 
    std::vector<uint16_t>& indices) {
    
    if (!state_.is_open || !has_font_) return;
    
    add_border(vertices, indices);
    add_text(vertices, indices);
}


void TextboxRenderer::render(VkCommandBuffer cmd) {
    if (!state_.is_open || !has_font_) return;
    
    std::vector<TextboxVertex> vertices;
    std::vector<uint16_t> indices;
    build_geometry(vertices, indices);
    
    if (vertices.empty()) return;
    
    void* data;
    vkMapMemory(device_, vertex_memory_, 0, vertices.size() * sizeof(TextboxVertex), 0, &data);
    memcpy(data, vertices.data(), vertices.size() * sizeof(TextboxVertex));
    vkUnmapMemory(device_, vertex_memory_);
    
    vkMapMemory(device_, index_memory_, 0, indices.size() * sizeof(uint16_t), 0, &data);
    memcpy(data, indices.data(), indices.size() * sizeof(uint16_t));
    vkUnmapMemory(device_, index_memory_);
    
    index_count_ = static_cast<uint32_t>(indices.size());
    
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(cmd, 0, 1, &scaled_viewport_);
    vkCmdSetScissor(cmd, 0, 1, &scaled_scissor_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 
        0, 1, &descriptor_set_, 0, nullptr);
    
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer_, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
}

void TextboxRenderer::destroy() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    
    if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
    if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);
    if (index_buffer_) vkDestroyBuffer(device_, index_buffer_, nullptr);
    if (index_memory_) vkFreeMemory(device_, index_memory_, nullptr);
    
    if (font_sampler_) vkDestroySampler(device_, font_sampler_, nullptr);
    if (font_view_) vkDestroyImageView(device_, font_view_, nullptr);
    if (font_image_) vkDestroyImage(device_, font_image_, nullptr);
    if (font_memory_) vkFreeMemory(device_, font_memory_, nullptr);
    
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    
    device_ = VK_NULL_HANDLE;
}

} // namespace enginemon
