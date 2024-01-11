#include "shader_vulkan.h"

#include "vulkan_common.h"
#include "slang_reflection.h"

#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

struct Texture
{
    vulkan_filter_chain_texture texture;
    glslang_filter_chain_filter filter;
    glslang_filter_chain_filter mip_filter;
    glslang_filter_chain_address address;
};

class DeferredDisposer
{
public:
    DeferredDisposer(std::vector<std::function<void()>>& calls) : calls(calls) {}

    void defer(std::function<void()> func)
    {
        calls.push_back(std::move(func));
    }

private:
    std::vector<std::function<void()>>& calls;
};

class Buffer
{
public:
    Buffer(VkDevice device,
        const VkPhysicalDeviceMemoryProperties& mem_props,
        size_t size, VkBufferUsageFlags usage);
    ~Buffer();

    size_t get_size() const { return size; }
    void* map();
    void unmap();

    const VkBuffer& get_buffer() const { return buffer; }

    Buffer(Buffer&&) = delete;
    void operator=(Buffer&&) = delete;

private:
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    size_t size;
    void* mapped = nullptr;
};

class Framebuffer
{
public:
    Framebuffer(VkDevice device,
        const VkPhysicalDeviceMemoryProperties& mem_props,
        const Size2D& max_size, VkFormat format, unsigned max_levels);

    ~Framebuffer();
    Framebuffer(Framebuffer&&) = delete;
    void operator=(Framebuffer&&) = delete;

    void set_size(DeferredDisposer& disposer, const Size2D& size, VkFormat format = VK_FORMAT_UNDEFINED);

    const Size2D& get_size() const { return size; }
    VkFormat get_format() const { return format; }
    VkImage get_image() const { return image; }
    VkImageView get_view() const { return view; }
    VkFramebuffer get_framebuffer() const { return framebuffer; }
    VkRenderPass get_render_pass() const { return render_pass; }

    unsigned get_levels() const { return levels; }

private:
    Size2D size;
    VkFormat format;
    unsigned max_levels;
    const VkPhysicalDeviceMemoryProperties& memory_properties;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView fb_view = VK_NULL_HANDLE;
    unsigned levels = 0;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;

    struct
    {
        size_t size = 0;
        uint32_t type = 0;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    } memory;

    void init(DeferredDisposer* disposer);
};

class StaticTexture
{
public:
    StaticTexture(std::string id,
        VkDevice device,
        VkImage image,
        VkImageView view,
        VkDeviceMemory memory,
        std::unique_ptr<Buffer> buffer,
        unsigned width, unsigned height,
        bool linear,
        bool mipmap,
        glslang_filter_chain_address address);
    ~StaticTexture();

    StaticTexture(StaticTexture&&) = delete;
    void operator=(StaticTexture&&) = delete;

    void release_staging_buffer() { buffer.reset(); }
    void set_id(std::string name) { id = std::move(name); }
    const std::string& get_id() const { return id; }
    const Texture& get_texture() const { return texture; }

private:
    VkDevice device;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    std::unique_ptr<Buffer> buffer;
    std::string id;
    Texture texture;
};

struct CommonResources
{
    CommonResources(VkDevice device,
        const VkPhysicalDeviceMemoryProperties& memory_properties);
    ~CommonResources();

    std::unique_ptr<Buffer> vbo;
    std::unique_ptr<Buffer> ubo;
    uint8_t* ubo_mapped = nullptr;
    size_t ubo_sync_index_stride = 0;
    size_t ubo_offset = 0;
    size_t ubo_alignment = 1;

    VkSampler samplers[GLSLANG_FILTER_CHAIN_COUNT][GLSLANG_FILTER_CHAIN_COUNT][GLSLANG_FILTER_CHAIN_ADDRESS_COUNT];

    std::vector<Texture> original_history;
    std::vector<Texture> fb_feedback;
    std::vector<Texture> pass_outputs;
    std::vector<std::unique_ptr<StaticTexture>> luts;

    std::unordered_map<std::string, slang_texture_semantic_map> texture_semantic_map;
    std::unordered_map<std::string, slang_texture_semantic_map> texture_semantic_uniform_map;
    std::unique_ptr<video_shader> shader_preset;

    VkDevice device;
};

class Pass
{
public:
    Pass(VkDevice device,
        const VkPhysicalDeviceMemoryProperties& memory_properties,
        VkPipelineCache cache, unsigned num_sync_indices, bool final_pass) :
        device(device),
        memory_properties(memory_properties),
        cache(cache),
        num_sync_indices(num_sync_indices),
        final_pass(final_pass)
    {}

    ~Pass();

    Pass(Pass&&) = delete;
    void operator=(Pass&&) = delete;

    const Framebuffer& get_framebuffer() const { return *framebuffer; }
    Framebuffer* get_feedback_framebuffer() { return fb_feedback.get(); }

    Size2D set_pass_info(
        const Size2D& max_original,
        const Size2D& max_source,
        const vulkan_filter_chain_swapchain_info& swapchain,
        const vulkan_filter_chain_pass_info& info);

    void set_shader(VkShaderStageFlags stage,
        const uint32_t* spirv,
        size_t spirv_words);

    bool build();
    bool init_feedback();

    void build_commands(
        DeferredDisposer& disposer,
        VkCommandBuffer cmd,
        const Texture& original,
        const Texture& source,
        const VkViewport& vp,
        const float* mvp);

    void notify_sync_index(unsigned index) { sync_index = index; }
    void set_frame_count(uint64_t count) { frame_count = count; }
    void set_frame_count_period(unsigned p) { frame_count_period = p; }
    void set_frame_direction(int32_t dir) { frame_direction = dir; }
    void set_name(const char* name) { pass_name = name; }
    const std::string& get_name() const { return pass_name; }
    glslang_filter_chain_filter get_source_filter() const {
        return pass_info.source_filter;
    }

    glslang_filter_chain_filter get_mip_filter() const
    {
        return pass_info.mip_filter;
    }

    glslang_filter_chain_address get_address_mode() const
    {
        return pass_info.address;
    }

    void set_common_resources(CommonResources* c) { this->common = c; }
    const slang_reflection& get_reflection() const { return reflection; }
    void set_pass_number(unsigned pass) { pass_number = pass; }

    void add_parameter(unsigned parameter_index, const std::string& id);

    void end_frame();
    void allocate_buffers();

private:
    VkDevice device;
    const VkPhysicalDeviceMemoryProperties& memory_properties;
    VkPipelineCache cache;
    unsigned num_sync_indices;
    unsigned sync_index;
    bool final_pass;

    Size2D get_output_size(const Size2D& original_size,
        const Size2D& max_source) const;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    std::vector<VkDescriptorSet> sets;
    CommonResources* common = nullptr;

    Size2D current_framebuffer_size;
    VkViewport current_viewport;
    vulkan_filter_chain_pass_info pass_info;

    std::vector<uint32_t> vertex_shader;
    std::vector<uint32_t> fragment_shader;
    std::unique_ptr<Framebuffer> framebuffer;
    std::unique_ptr<Framebuffer> fb_feedback;
    VkRenderPass swapchain_render_pass;

    void clear_vk();
    bool init_pipeline();
    bool init_pipeline_layout();

    void set_semantic_texture(VkDescriptorSet set,
        slang_texture_semantic semantic,
        const Texture& texture);
    void set_semantic_texture_array(VkDescriptorSet set,
        slang_texture_semantic semantic, unsigned index,
        const Texture& texture);

    slang_reflection reflection;
    void build_semantics(VkDescriptorSet set, uint8_t* buffer,
        const float* mvp, const Texture& original, const Texture& source);
    void build_semantic_vec4(uint8_t* data, slang_semantic semantic,
        unsigned width, unsigned height);
    void build_semantic_uint(uint8_t* data, slang_semantic semantic, uint32_t value);
    void build_semantic_int(uint8_t* data, slang_semantic semantic, int32_t value);
    void build_semantic_parameter(uint8_t* data, unsigned index, float value);
    void build_semantic_texture_vec4(uint8_t* data,
        slang_texture_semantic semantic,
        unsigned width, unsigned height);
    void build_semantic_texture_array_vec4(uint8_t* data,
        slang_texture_semantic semantic, unsigned index,
        unsigned width, unsigned height);
    void build_semantic_texture(VkDescriptorSet set, uint8_t* buffer,
        slang_texture_semantic semantic, const Texture& texture);
    void build_semantic_texture_array(VkDescriptorSet set, uint8_t* buffer,
        slang_texture_semantic semantic, unsigned index, const Texture& texture);

    uint64_t frame_count = 0;
    int32_t frame_direction = 1;
    unsigned frame_count_period = 0;
    unsigned pass_number = 0;

    size_t ubo_offset = 0;
    std::string pass_name;

    struct Parameter
    {
        std::string id;
        unsigned index;
        unsigned semantic_index;
    };

    std::vector<Parameter> parameters;
    std::vector<Parameter> filtered_parameters;

    struct PushConstant
    {
        VkShaderStageFlags stages = 0;
        std::vector<uint32_t> buffer; /* uint32_t to have correct alignment. */
    };
    PushConstant push;
};

/* struct here since we're implementing the opaque typedef from C. */
struct vulkan_filter_chain
{
public:
    vulkan_filter_chain(const vulkan_filter_chain_create_info& info);
    ~vulkan_filter_chain();

    inline void set_shader_preset(std::unique_ptr<video_shader> shader)
    {
        common.shader_preset = std::move(shader);
    }

    inline video_shader* get_shader_preset()
    {
        return common.shader_preset.get();
    }

    void set_pass_info(unsigned pass,
        const vulkan_filter_chain_pass_info& info);
    void set_shader(unsigned pass, VkShaderStageFlags stage,
        const uint32_t* spirv, size_t spirv_words);

    bool init();
    bool update_swapchain_info(
        const vulkan_filter_chain_swapchain_info& info);

    void notify_sync_index(unsigned index);
    void set_input_texture(const vulkan_filter_chain_texture& texture);
    void build_offscreen_passes(VkCommandBuffer cmd, const VkViewport& vp);
    void build_viewport_pass(VkCommandBuffer cmd,
        const VkViewport& vp, const float* mvp);
    void end_frame(VkCommandBuffer cmd);

    void set_frame_count(uint64_t count);
    void set_frame_count_period(unsigned pass, unsigned period);
    void set_frame_direction(int32_t direction);
    void set_pass_name(unsigned pass, const char* name);

    void add_static_texture(std::unique_ptr<StaticTexture> texture);
    void add_parameter(unsigned pass, unsigned parameter_index, const std::string& id);
    void release_staging_buffers();

    VkFormat get_pass_rt_format(unsigned pass);

private:
    VkDevice device;
    VkPhysicalDevice gpu;
    const VkPhysicalDeviceMemoryProperties& memory_properties;
    VkPipelineCache cache;
    std::vector<std::unique_ptr<Pass>> passes;
    std::vector<vulkan_filter_chain_pass_info> pass_info;
    std::vector<std::vector<std::function<void()>>> deferred_calls;
    CommonResources common;
    VkFormat original_format;

    vulkan_filter_chain_texture input_texture;

    Size2D max_input_size;
    vulkan_filter_chain_swapchain_info swapchain_info;
    unsigned current_sync_index;

    void flush();

    void set_num_passes(unsigned passes);
    void execute_deferred();
    void set_num_sync_indices(unsigned num_indices);
    void set_swapchain_info(const vulkan_filter_chain_swapchain_info& info);

    bool init_ubo();
    bool init_history();
    bool init_feedback();
    bool init_alias();
    void update_history(DeferredDisposer& disposer, VkCommandBuffer cmd);
    std::vector<std::unique_ptr<Framebuffer>> original_history;
    bool require_clear = false;
    void clear_history_and_feedback(VkCommandBuffer cmd);
    void update_feedback_info();
    void update_history_info();
};

static uint32_t find_memory_type_fallback(
    const VkPhysicalDeviceMemoryProperties& mem_props,
    uint32_t device_reqs, uint32_t host_reqs)
{
    unsigned i;
    for (i = 0; i < VK_MAX_MEMORY_TYPES; i++)
    {
        if ((device_reqs & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & host_reqs) == host_reqs)
            return i;
    }

    return vulkan_find_memory_type(&mem_props, device_reqs, 0);
}


CommonResources::CommonResources(VkDevice device,
    const VkPhysicalDeviceMemoryProperties& memory_properties)
    : device(device)
{
    unsigned i;
    VkSamplerCreateInfo info = {
       VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    /* The final pass uses an MVP designed for [0, 1] range VBO.
     * For in-between passes, we just go with identity matrices,
     * so keep it simple.
     */
    const float vbo_data[] = {
        /* Offscreen */
        -1.0f, -1.0f, 0.0f, 0.0f,
        -1.0f, +1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f, +1.0f, 1.0f, 1.0f,

         /* Final */
        0.0f,  0.0f, 0.0f, 0.0f,
        0.0f, +1.0f, 0.0f, 1.0f,
        1.0f,  0.0f, 1.0f, 0.0f,
        1.0f, +1.0f, 1.0f, 1.0f,
    };

    vbo =
        std::unique_ptr<Buffer>(new Buffer(device,
            memory_properties, sizeof(vbo_data), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));

    void* ptr = vbo->map();
    memcpy(ptr, vbo_data, sizeof(vbo_data));
    vbo->unmap();

    info.mipLodBias = 0.0f;
    info.maxAnisotropy = 1.0f;
    info.compareEnable = false;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.unnormalizedCoordinates = false;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    for (i = 0; i < GLSLANG_FILTER_CHAIN_COUNT; i++)
    {
        unsigned j;

        switch (static_cast<glslang_filter_chain_filter>(i))
        {
        case GLSLANG_FILTER_CHAIN_LINEAR:
            info.magFilter = VK_FILTER_LINEAR;
            info.minFilter = VK_FILTER_LINEAR;
            break;

        case GLSLANG_FILTER_CHAIN_NEAREST:
            info.magFilter = VK_FILTER_NEAREST;
            info.minFilter = VK_FILTER_NEAREST;
            break;

        default:
            break;
        }

        for (j = 0; j < GLSLANG_FILTER_CHAIN_COUNT; j++)
        {
            unsigned k;

            switch (static_cast<glslang_filter_chain_filter>(j))
            {
            case GLSLANG_FILTER_CHAIN_LINEAR:
                info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;

            case GLSLANG_FILTER_CHAIN_NEAREST:
                info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                break;

            default:
                break;
            }

            for (k = 0; k < GLSLANG_FILTER_CHAIN_ADDRESS_COUNT; k++)
            {
                VkSamplerAddressMode mode = VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;

                switch (static_cast<glslang_filter_chain_address>(k))
                {
                case GLSLANG_FILTER_CHAIN_ADDRESS_REPEAT:
                    mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                    break;

                case GLSLANG_FILTER_CHAIN_ADDRESS_MIRRORED_REPEAT:
                    mode = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                    break;

                case GLSLANG_FILTER_CHAIN_ADDRESS_CLAMP_TO_EDGE:
                    mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    break;

                case GLSLANG_FILTER_CHAIN_ADDRESS_CLAMP_TO_BORDER:
                    mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                    break;

                case GLSLANG_FILTER_CHAIN_ADDRESS_MIRROR_CLAMP_TO_EDGE:
                    mode = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
                    break;

                default:
                    break;
                }

                info.addressModeU = mode;
                info.addressModeV = mode;
                info.addressModeW = mode;
                vkCreateSampler(device, &info, nullptr, &samplers[i][j][k]);
            }
        }
    }
}

CommonResources::~CommonResources()
{
    for (auto& i : samplers)
        for (auto& j : i)
            for (auto& k : j)
                if (k != VK_NULL_HANDLE)
                    vkDestroySampler(device, k, nullptr);
}

void Framebuffer::set_size(DeferredDisposer& disposer, const Size2D& size, VkFormat format)
{
    this->size = size;
    if (format != VK_FORMAT_UNDEFINED)
        this->format = format;

    RARCH_LOG("[Vulkan filter chain]: Updating framebuffer size %ux%u (format: %u).\n",
        size.width, size.height, (unsigned)this->format);

    {
        /* The current framebuffers, etc, might still be in use
         * so defer deletion.
         * We'll most likely be able to reuse the memory,
         * so don't free it here.
         *
         * Fake lambda init captures for C++11.
         */
        VkDevice d = device;
        VkImage i = image;
        VkImageView v = view;
        VkImageView fbv = fb_view;
        VkFramebuffer fb = framebuffer;
        disposer.defer([=]
            {
                if (fb != VK_NULL_HANDLE)
                    vkDestroyFramebuffer(d, fb, nullptr);
                if (v != VK_NULL_HANDLE)
                    vkDestroyImageView(d, v, nullptr);
                if (fbv != VK_NULL_HANDLE)
                    vkDestroyImageView(d, fbv, nullptr);
                if (i != VK_NULL_HANDLE)
                    vkDestroyImage(d, i, nullptr);
            });
    }

    init(&disposer);
}

void Framebuffer::init(DeferredDisposer* disposer)
{
    VkMemoryRequirements mem_reqs;
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo view_info = {
       VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };

    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = size.width;
    info.extent.height = size.height;
    info.extent.depth = 1;
    info.mipLevels = 0; // std::min(max_levels, glslang_num_miplevels(size.width, size.height));
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    levels = info.mipLevels;

    vkCreateImage(device, &info, nullptr, &image);

    vkGetImageMemoryRequirements(device, image, &mem_reqs);

    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = find_memory_type_fallback(
        memory_properties, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    /* Can reuse already allocated memory. */
    if (memory.size < mem_reqs.size || memory.type != alloc.memoryTypeIndex)
    {
        /* Memory might still be in use since we don't want
         * to totally stall
         * the world for framebuffer recreation. */
        if (memory.memory != VK_NULL_HANDLE && disposer)
        {
            VkDevice       d = device;
            VkDeviceMemory m = memory.memory;
            disposer->defer([=] { vkFreeMemory(d, m, nullptr); });
        }

        memory.type = alloc.memoryTypeIndex;
        memory.size = mem_reqs.size;

        vkAllocateMemory(device, &alloc, nullptr, &memory.memory);
    }

    vkBindImageMemory(device, image, memory.memory, 0);

    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.image = image;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.levelCount = levels;
    view_info.subresourceRange.layerCount = 1;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.components.r = VK_COMPONENT_SWIZZLE_R;
    view_info.components.g = VK_COMPONENT_SWIZZLE_G;
    view_info.components.b = VK_COMPONENT_SWIZZLE_B;
    view_info.components.a = VK_COMPONENT_SWIZZLE_A;

    vkCreateImageView(device, &view_info, nullptr, &view);
    view_info.subresourceRange.levelCount = 1;
    vkCreateImageView(device, &view_info, nullptr, &fb_view);

    /* Initialize framebuffer */
    {
        VkFramebufferCreateInfo info = {
           VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        info.renderPass = render_pass;
        info.attachmentCount = 1;
        info.pAttachments = &fb_view;
        info.width = size.width;
        info.height = size.height;
        info.layers = 1;

        vkCreateFramebuffer(device, &info, nullptr, &framebuffer);
    }
}

Framebuffer::Framebuffer(
    VkDevice device,
    const VkPhysicalDeviceMemoryProperties& mem_props,
    const Size2D& max_size, VkFormat format,
    unsigned max_levels) :
    size(max_size),
    format(format),
    max_levels(std::max(max_levels, 1u)),
    memory_properties(mem_props),
    device(device)
{
    RARCH_LOG("[Vulkan filter chain]: Creating framebuffer %ux%u (max %u level(s)).\n",
        max_size.width, max_size.height, max_levels);
    vulkan_initialize_render_pass(device, format, &render_pass);
    init(nullptr);
}

Framebuffer::~Framebuffer()
{
    if (framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (render_pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(device, render_pass, nullptr);
    if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    if (fb_view != VK_NULL_HANDLE)
        vkDestroyImageView(device, fb_view, nullptr);
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(device, image, nullptr);
    if (memory.memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory.memory, nullptr);
}

StaticTexture::~StaticTexture()
{
    if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(device, image, nullptr);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory, nullptr);
}

Buffer::Buffer(VkDevice device,
    const VkPhysicalDeviceMemoryProperties& mem_props,
    size_t size, VkBufferUsageFlags usage) :
    device(device), size(size)
{
    VkMemoryRequirements mem_reqs;
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };

    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &info, nullptr, &buffer);

    vkGetBufferMemoryRequirements(device, buffer, &mem_reqs);

    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = vulkan_find_memory_type(
        &mem_props, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(device, &alloc, NULL, &memory);
    vkBindBufferMemory(device, buffer, memory, 0);
}

Buffer::~Buffer()
{
    if (mapped)
        unmap();
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device, memory, nullptr);
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, buffer, nullptr);
}

void* Buffer::map()
{
    if (!mapped)
    {
        if (vkMapMemory(device, memory, 0, size, 0, &mapped) != VK_SUCCESS)
            return nullptr;
    }
    return mapped;
}

void Buffer::unmap()
{
    if (mapped)
        vkUnmapMemory(device, memory);
    mapped = nullptr;
}

Pass::~Pass()
{
    clear_vk();
}

bool Pass::init_pipeline_layout()
{
    unsigned i;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorPoolSize> desc_counts;
    VkPushConstantRange push_range = {};
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
       VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo layout_info = {
       VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorPoolCreateInfo pool_info = {
       VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo alloc_info = {
       VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };

    /* Main UBO. */
    VkShaderStageFlags ubo_mask = 0;

    if (reflection.ubo_stage_mask & SLANG_STAGE_VERTEX_MASK)
        ubo_mask |= VK_SHADER_STAGE_VERTEX_BIT;
    if (reflection.ubo_stage_mask & SLANG_STAGE_FRAGMENT_MASK)
        ubo_mask |= VK_SHADER_STAGE_FRAGMENT_BIT;

    if (ubo_mask != 0)
    {
        bindings.push_back({ reflection.ubo_binding,
              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
              ubo_mask, nullptr });
        desc_counts.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, num_sync_indices });
    }

    /* Semantic textures. */
    for (auto& semantic : reflection.semantic_textures)
    {
        for (auto& texture : semantic)
        {
            VkShaderStageFlags stages = 0;

            if (!texture.texture)
                continue;

            if (texture.stage_mask & SLANG_STAGE_VERTEX_MASK)
                stages |= VK_SHADER_STAGE_VERTEX_BIT;
            if (texture.stage_mask & SLANG_STAGE_FRAGMENT_MASK)
                stages |= VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings.push_back({ texture.binding,
                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                  stages, nullptr });
            desc_counts.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, num_sync_indices });
        }
    }

    set_layout_info.bindingCount = bindings.size();
    set_layout_info.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device,
        &set_layout_info, NULL, &set_layout) != VK_SUCCESS)
        return false;

    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;

    /* Push constants */
    if (reflection.push_constant_stage_mask && reflection.push_constant_size)
    {
        if (reflection.push_constant_stage_mask & SLANG_STAGE_VERTEX_MASK)
            push_range.stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        if (reflection.push_constant_stage_mask & SLANG_STAGE_FRAGMENT_MASK)
            push_range.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;

#ifdef VULKAN_DEBUG
        RARCH_LOG("[Vulkan]: Push Constant Block: %u bytes.\n", (unsigned int)reflection.push_constant_size);
#endif

        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
        push.buffer.resize((reflection.push_constant_size + sizeof(uint32_t) - 1) / sizeof(uint32_t));
    }

    push.stages = push_range.stageFlags;
    push_range.size = reflection.push_constant_size;

    if (vkCreatePipelineLayout(device,
        &layout_info, NULL, &pipeline_layout) != VK_SUCCESS)
        return false;

    pool_info.maxSets = num_sync_indices;
    pool_info.poolSizeCount = desc_counts.size();
    pool_info.pPoolSizes = desc_counts.data();
    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS)
        return false;

    alloc_info.descriptorPool = pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &set_layout;

    sets.resize(num_sync_indices);

    for (i = 0; i < num_sync_indices; i++)
        vkAllocateDescriptorSets(device, &alloc_info, &sets[i]);

    return true;
}

bool Pass::init_pipeline()
{
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
       VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkVertexInputAttributeDescription attributes[2] = { {0} };
    VkVertexInputBindingDescription binding = { 0 };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
       VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo raster = {
       VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = {
       VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo viewport = {
       VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
       VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo multisample = {
       VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dynamic = {
       VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    static const VkDynamicState dynamics[] = {
       VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineShaderStageCreateInfo shader_stages[2] = {
       { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
       { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkShaderModuleCreateInfo module_info = {
       VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo pipe = {
       VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };

    if (!init_pipeline_layout())
        return false;

    /* Input assembly */
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    /* VAO state */
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = 2 * sizeof(float);

    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    /* Raster state */
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace =
        VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthClampEnable = false;
    raster.rasterizerDiscardEnable = false;
    raster.depthBiasEnable = false;
    raster.lineWidth = 1.0f;

    /* Blend state */
    blend_attachment.blendEnable = false;
    blend_attachment.colorWriteMask = 0xf;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    /* Viewport state */
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    /* Depth-stencil state */
    depth_stencil.depthTestEnable = false;
    depth_stencil.depthWriteEnable = false;
    depth_stencil.depthBoundsTestEnable = false;
    depth_stencil.stencilTestEnable = false;
    depth_stencil.minDepthBounds = 0.0f;
    depth_stencil.maxDepthBounds = 1.0f;

    /* Multisample state */
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Dynamic state */
    dynamic.pDynamicStates = dynamics;
    dynamic.dynamicStateCount = sizeof(dynamics) / sizeof(dynamics[0]);

    /* Shaders */
    module_info.codeSize = vertex_shader.size() * sizeof(uint32_t);
    module_info.pCode = vertex_shader.data();
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].pName = "main";
    vkCreateShaderModule(device, &module_info, NULL, &shader_stages[0].module);

    module_info.codeSize = fragment_shader.size() * sizeof(uint32_t);
    module_info.pCode = fragment_shader.data();
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].pName = "main";
    vkCreateShaderModule(device, &module_info, NULL, &shader_stages[1].module);

    pipe.stageCount = 2;
    pipe.pStages = shader_stages;
    pipe.pVertexInputState = &vertex_input;
    pipe.pInputAssemblyState = &input_assembly;
    pipe.pRasterizationState = &raster;
    pipe.pColorBlendState = &blend;
    pipe.pMultisampleState = &multisample;
    pipe.pViewportState = &viewport;
    pipe.pDepthStencilState = &depth_stencil;
    pipe.pDynamicState = &dynamic;
    pipe.renderPass = final_pass ? swapchain_render_pass :
        framebuffer->get_render_pass();
    pipe.layout = pipeline_layout;

    if (vkCreateGraphicsPipelines(device,
        cache, 1, &pipe, NULL, &pipeline) != VK_SUCCESS)
    {
        vkDestroyShaderModule(device, shader_stages[0].module, NULL);
        vkDestroyShaderModule(device, shader_stages[1].module, NULL);
        return false;
    }

    vkDestroyShaderModule(device, shader_stages[0].module, NULL);
    vkDestroyShaderModule(device, shader_stages[1].module, NULL);
    return true;
}

bool Pass::build()
{
    unsigned i;
    unsigned j = 0;

    framebuffer.reset();
    fb_feedback.reset();

    if (!final_pass)
        framebuffer = std::unique_ptr<Framebuffer>(
            new Framebuffer(device, memory_properties,
                current_framebuffer_size,
                pass_info.rt_format, pass_info.max_levels));

    std::unordered_map<std::string, slang_semantic_map> semantic_map;
    for (i = 0; i < parameters.size(); i++)
    {
        if (!slang_set_unique_map(
            semantic_map, parameters[i].id,
            slang_semantic_map{ SLANG_SEMANTIC_FLOAT_PARAMETER, j }))
            return false;
        j++;
    }

    reflection = slang_reflection{};
    reflection.pass_number = pass_number;
    reflection.texture_semantic_map = &common->texture_semantic_map;
    reflection.texture_semantic_uniform_map = &common->texture_semantic_uniform_map;
    reflection.semantic_map = &semantic_map;

    if (!slang_reflect_spirv(vertex_shader, fragment_shader, &reflection))
        return false;

    /* Filter out parameters which we will never use anyways. */
    filtered_parameters.clear();

    for (i = 0; i < reflection.semantic_float_parameters.size(); i++)
    {
        if (reflection.semantic_float_parameters[i].uniform ||
            reflection.semantic_float_parameters[i].push_constant)
            filtered_parameters.push_back(parameters[i]);
    }

    return init_pipeline();
}

Size2D Pass::set_pass_info(
    const Size2D& max_original,
    const Size2D& max_source,
    const vulkan_filter_chain_swapchain_info& swapchain,
    const vulkan_filter_chain_pass_info& info)
{
    clear_vk();

    current_viewport = swapchain.viewport;
    pass_info = info;

    num_sync_indices = swapchain.num_indices;
    sync_index = 0;

    current_framebuffer_size = get_output_size(max_original, max_source);
    swapchain_render_pass = swapchain.render_pass;

    return current_framebuffer_size;
}

Size2D Pass::get_output_size(const Size2D& original,
    const Size2D& source) const
{
    float width = 0.0f;
    float height = 0.0f;
    switch (pass_info.scale_type_x)
    {
    case GLSLANG_FILTER_CHAIN_SCALE_ORIGINAL:
        width = float(original.width) * pass_info.scale_x;
        break;

    case GLSLANG_FILTER_CHAIN_SCALE_SOURCE:
        width = float(source.width) * pass_info.scale_x;
        break;

    case GLSLANG_FILTER_CHAIN_SCALE_VIEWPORT:
        width = current_viewport.width * pass_info.scale_x;
        break;

    case GLSLANG_FILTER_CHAIN_SCALE_ABSOLUTE:
        width = pass_info.scale_x;
        break;

    default:
        break;
    }

    switch (pass_info.scale_type_y)
    {
    case GLSLANG_FILTER_CHAIN_SCALE_ORIGINAL:
        height = float(original.height) * pass_info.scale_y;
        break;

    case GLSLANG_FILTER_CHAIN_SCALE_SOURCE:
        height = float(source.height) * pass_info.scale_y;
        break;

    case GLSLANG_FILTER_CHAIN_SCALE_VIEWPORT:
        height = current_viewport.height * pass_info.scale_y;
        break;

    case GLSLANG_FILTER_CHAIN_SCALE_ABSOLUTE:
        height = pass_info.scale_y;
        break;

    default:
        break;
    }

    return { unsigned(roundf(width)), unsigned(roundf(height)) };
}

void Pass::clear_vk()
{
    if (pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device, pool, nullptr);
    if (pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline, nullptr);
    if (set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    if (pipeline_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);

    pool = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
}

void Pass::allocate_buffers()
{
    if (reflection.ubo_stage_mask)
    {
        /* Align */
        common->ubo_offset = (common->ubo_offset + common->ubo_alignment - 1) &
            ~(common->ubo_alignment - 1);
        ubo_offset = common->ubo_offset;

        /* Allocate */
        common->ubo_offset += reflection.ubo_size;
    }
}

void Pass::set_shader(VkShaderStageFlags stage,
    const uint32_t* spirv,
    size_t spirv_words)
{
    switch (stage)
    {
    case VK_SHADER_STAGE_VERTEX_BIT:
        vertex_shader.clear();
        vertex_shader.insert(end(vertex_shader),
            spirv, spirv + spirv_words);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        fragment_shader.clear();
        fragment_shader.insert(end(fragment_shader),
            spirv, spirv + spirv_words);
        break;
    default:
        break;
    }
}

void Pass::build_commands(
    DeferredDisposer& disposer,
    VkCommandBuffer cmd,
    const Texture& original,
    const Texture& source,
    const VkViewport& vp,
    const float* mvp)
{
    uint8_t* u = nullptr;

    current_viewport = vp;
    Size2D size = get_output_size(
        { original.texture.width, original.texture.height },
        { source.texture.width, source.texture.height });

    if (framebuffer &&
        (size.width != framebuffer->get_size().width ||
            size.height != framebuffer->get_size().height))
        framebuffer->set_size(disposer, size);

    current_framebuffer_size = size;

    if (reflection.ubo_stage_mask && common->ubo_mapped)
        u = common->ubo_mapped + ubo_offset +
        sync_index * common->ubo_sync_index_stride;

    build_semantics(sets[sync_index], u, mvp, original, source);

    if (reflection.ubo_stage_mask)
        vulkan_set_uniform_buffer(device,
            sets[sync_index],
            reflection.ubo_binding,
            common->ubo->get_buffer(),
            ubo_offset + sync_index * common->ubo_sync_index_stride,
            reflection.ubo_size);

    /* The final pass is always executed inside
     * another render pass since the frontend will
     * want to overlay various things on top for
     * the passes that end up on-screen. */
    if (!final_pass)
    {
        VkRenderPassBeginInfo rp_info;

        /* Render. */
        VULKAN_IMAGE_LAYOUT_TRANSITION_LEVELS(cmd,
            framebuffer->get_image(), 1,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            0,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED);

        rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.pNext = NULL;
        rp_info.renderPass = framebuffer->get_render_pass();
        rp_info.framebuffer = framebuffer->get_framebuffer();
        rp_info.renderArea.offset.x = 0;
        rp_info.renderArea.offset.y = 0;
        rp_info.renderArea.extent.width = current_framebuffer_size.width;
        rp_info.renderArea.extent.height = current_framebuffer_size.height;
        rp_info.clearValueCount = 0;
        rp_info.pClearValues = nullptr;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout,
        0, 1, &sets[sync_index], 0, nullptr);

    if (push.stages != 0)
    {
        vkCmdPushConstants(cmd, pipeline_layout,
            push.stages, 0, reflection.push_constant_size,
            push.buffer.data());
    }

    {
        VkDeviceSize offset = final_pass ? 16 * sizeof(float) : 0;
        vkCmdBindVertexBuffers(cmd, 0, 1,
            &common->vbo->get_buffer(),
            &offset);
    }

    if (final_pass)
    {
        const VkRect2D sci = {
           {
              int32_t(current_viewport.x),
              int32_t(current_viewport.y)
           },
           {
              uint32_t(current_viewport.width),
              uint32_t(current_viewport.height)
           },
        };
        vkCmdSetViewport(cmd, 0, 1, &current_viewport);
        vkCmdSetScissor(cmd, 0, 1, &sci);
    }
    else
    {
        const VkViewport _vp = {
           0.0f, 0.0f,
           float(current_framebuffer_size.width),
           float(current_framebuffer_size.height),
           0.0f, 1.0f
        };
        const VkRect2D sci = {
           { 0, 0 },
           {
              current_framebuffer_size.width,
              current_framebuffer_size.height
           },
        };

        vkCmdSetViewport(cmd, 0, 1, &_vp);
        vkCmdSetScissor(cmd, 0, 1, &sci);
    }

    vkCmdDraw(cmd, 4, 1, 0, 0);

    if (!final_pass)
    {
        vkCmdEndRenderPass(cmd);

        if (framebuffer->get_levels() > 1)
            vulkan_framebuffer_generate_mips(
                framebuffer->get_framebuffer(),
                framebuffer->get_image(),
                framebuffer->get_size(),
                cmd,
                framebuffer->get_levels());
        else
        {
            /* Barrier to sync with next pass. */
            VULKAN_IMAGE_LAYOUT_TRANSITION_LEVELS(
                cmd,
                framebuffer->get_image(),
                VK_REMAINING_MIP_LEVELS,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED);
        }
    }
}

void Pass::end_frame()
{
    if (fb_feedback)
        swap(framebuffer, fb_feedback);
}

static void build_identity_matrix(float* data)
{
    data[0] = 1.0f;
    data[1] = 0.0f;
    data[2] = 0.0f;
    data[3] = 0.0f;
    data[4] = 0.0f;
    data[5] = 1.0f;
    data[6] = 0.0f;
    data[7] = 0.0f;
    data[8] = 0.0f;
    data[9] = 0.0f;
    data[10] = 1.0f;
    data[11] = 0.0f;
    data[12] = 0.0f;
    data[13] = 0.0f;
    data[14] = 0.0f;
    data[15] = 1.0f;
}

void Pass::build_semantics(VkDescriptorSet set, uint8_t* buffer,
    const float* mvp, const Texture& original, const Texture& source)
{
    unsigned i;

    /* MVP */
    if (buffer && reflection.semantics[SLANG_SEMANTIC_MVP].uniform)
    {
        size_t offset = reflection.semantics[SLANG_SEMANTIC_MVP].ubo_offset;
        if (mvp)
            memcpy(buffer + offset, mvp, sizeof(float) * 16);
        else
            build_identity_matrix(reinterpret_cast<float*>(buffer + offset));
    }

    if (reflection.semantics[SLANG_SEMANTIC_MVP].push_constant)
    {
        size_t offset = reflection.semantics[SLANG_SEMANTIC_MVP].push_constant_offset;
        if (mvp)
            memcpy(push.buffer.data() + (offset >> 2), mvp, sizeof(float) * 16);
        else
            build_identity_matrix(reinterpret_cast<float*>(push.buffer.data() + (offset >> 2)));
    }

    /* Output information */
    build_semantic_vec4(buffer, SLANG_SEMANTIC_OUTPUT,
        current_framebuffer_size.width,
        current_framebuffer_size.height);
    build_semantic_vec4(buffer, SLANG_SEMANTIC_FINAL_VIEWPORT,
        unsigned(current_viewport.width),
        unsigned(current_viewport.height));

    build_semantic_uint(buffer, SLANG_SEMANTIC_FRAME_COUNT,
        frame_count_period
        ? uint32_t(frame_count % frame_count_period)
        : uint32_t(frame_count));

    build_semantic_int(buffer, SLANG_SEMANTIC_FRAME_DIRECTION,
        frame_direction);

    /* Standard inputs */
    build_semantic_texture(set, buffer, SLANG_TEXTURE_SEMANTIC_ORIGINAL, original);
    build_semantic_texture(set, buffer, SLANG_TEXTURE_SEMANTIC_SOURCE, source);

    /* ORIGINAL_HISTORY[0] is an alias of ORIGINAL. */
    build_semantic_texture_array(set, buffer,
        SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY, 0, original);

    /* Parameters. */
    for (i = 0; i < filtered_parameters.size(); i++)
        build_semantic_parameter(buffer,
            filtered_parameters[i].semantic_index,
            common->shader_preset->parameters[
                filtered_parameters[i].index].current);

    /* Previous inputs. */
    for (i = 0; i < common->original_history.size(); i++)
        build_semantic_texture_array(set, buffer,
            SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY, i + 1,
            common->original_history[i]);

    /* Previous passes. */
    for (i = 0; i < common->pass_outputs.size(); i++)
        build_semantic_texture_array(set, buffer,
            SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i,
            common->pass_outputs[i]);

    /* Feedback FBOs. */
    for (i = 0; i < common->fb_feedback.size(); i++)
        build_semantic_texture_array(set, buffer,
            SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i,
            common->fb_feedback[i]);

    /* LUTs. */
    for (i = 0; i < common->luts.size(); i++)
        build_semantic_texture_array(set, buffer,
            SLANG_TEXTURE_SEMANTIC_USER, i,
            common->luts[i]->get_texture());
}

void Pass::set_semantic_texture(VkDescriptorSet set,
    slang_texture_semantic semantic, const Texture& texture)
{
    if (reflection.semantic_textures[semantic][0].texture)
    {
        VULKAN_PASS_SET_TEXTURE(device, set, common->samplers[texture.filter][texture.mip_filter][texture.address], reflection.semantic_textures[semantic][0].binding, texture.texture.view, texture.texture.layout);
    }
}

void Pass::set_semantic_texture_array(VkDescriptorSet set,
    slang_texture_semantic semantic, unsigned index,
    const Texture& texture)
{
    if (index < reflection.semantic_textures[semantic].size() &&
        reflection.semantic_textures[semantic][index].texture)
    {
        VULKAN_PASS_SET_TEXTURE(device, set, common->samplers[texture.filter][texture.mip_filter][texture.address], reflection.semantic_textures[semantic][index].binding, texture.texture.view, texture.texture.layout);
    }
}

void Pass::build_semantic_texture_array_vec4(uint8_t* data, slang_texture_semantic semantic,
    unsigned index, unsigned width, unsigned height)
{
    auto& refl = reflection.semantic_textures[semantic];

    if (index >= refl.size())
        return;

    if (data && refl[index].uniform)
    {
        float* _data = reinterpret_cast<float*>(data + refl[index].ubo_offset);
        _data[0] = (float)(width);
        _data[1] = (float)(height);
        _data[2] = 1.0f / (float)(width);
        _data[3] = 1.0f / (float)(height);
    }

    if (refl[index].push_constant)
    {
        float* _data = reinterpret_cast<float*>(push.buffer.data() + (refl[index].push_constant_offset >> 2));
        _data[0] = (float)(width);
        _data[1] = (float)(height);
        _data[2] = 1.0f / (float)(width);
        _data[3] = 1.0f / (float)(height);
    }
}

void Pass::build_semantic_texture_vec4(uint8_t* data, slang_texture_semantic semantic,
    unsigned width, unsigned height)
{
    build_semantic_texture_array_vec4(data, semantic, 0, width, height);
}

void Pass::build_semantic_vec4(uint8_t* data, slang_semantic semantic,
    unsigned width, unsigned height)
{
    auto& refl = reflection.semantics[semantic];

    if (data && refl.uniform)
    {
        float* _data = reinterpret_cast<float*>(data + refl.ubo_offset);
        _data[0] = (float)(width);
        _data[1] = (float)(height);
        _data[2] = 1.0f / (float)(width);
        _data[3] = 1.0f / (float)(height);
    }

    if (refl.push_constant)
    {
        float* _data = reinterpret_cast<float*>
            (push.buffer.data() + (refl.push_constant_offset >> 2));
        _data[0] = (float)(width);
        _data[1] = (float)(height);
        _data[2] = 1.0f / (float)(width);
        _data[3] = 1.0f / (float)(height);
    }
}

void Pass::build_semantic_parameter(uint8_t* data, unsigned index, float value)
{
    auto& refl = reflection.semantic_float_parameters[index];

    /* We will have filtered out stale parameters. */
    if (data && refl.uniform)
        *reinterpret_cast<float*>(data + refl.ubo_offset) = value;

    if (refl.push_constant)
        *reinterpret_cast<float*>(push.buffer.data() + (refl.push_constant_offset >> 2)) = value;
}

void Pass::build_semantic_uint(uint8_t* data, slang_semantic semantic,
    uint32_t value)
{
    auto& refl = reflection.semantics[semantic];

    if (data && refl.uniform)
        *reinterpret_cast<uint32_t*>(data + reflection.semantics[semantic].ubo_offset) = value;

    if (refl.push_constant)
        *reinterpret_cast<uint32_t*>(push.buffer.data() + (refl.push_constant_offset >> 2)) = value;
}

void Pass::build_semantic_int(uint8_t* data, slang_semantic semantic,
    int32_t value)
{
    auto& refl = reflection.semantics[semantic];

    if (data && refl.uniform)
        *reinterpret_cast<int32_t*>(data + reflection.semantics[semantic].ubo_offset) = value;

    if (refl.push_constant)
        *reinterpret_cast<int32_t*>(push.buffer.data() + (refl.push_constant_offset >> 2)) = value;
}

void Pass::build_semantic_texture(VkDescriptorSet set, uint8_t* buffer,
    slang_texture_semantic semantic, const Texture& texture)
{
    build_semantic_texture_vec4(buffer, semantic,
        texture.texture.width, texture.texture.height);
    set_semantic_texture(set, semantic, texture);
}

void Pass::build_semantic_texture_array(VkDescriptorSet set, uint8_t* buffer,
    slang_texture_semantic semantic, unsigned index, const Texture& texture)
{
    build_semantic_texture_array_vec4(buffer, semantic, index,
        texture.texture.width, texture.texture.height);
    set_semantic_texture_array(set, semantic, index, texture);
}

bool vulkan_filter_chain::init_ubo()
{
    unsigned i;
    VkPhysicalDeviceProperties props;

    common.ubo.reset();
    common.ubo_offset = 0;

    vkGetPhysicalDeviceProperties(gpu, &props);
    common.ubo_alignment = props.limits.minUniformBufferOffsetAlignment;

    /* Who knows. :) */
    if (common.ubo_alignment == 0)
        common.ubo_alignment = 1;

    for (i = 0; i < passes.size(); i++)
        passes[i]->allocate_buffers();

    common.ubo_offset =
        (common.ubo_offset + common.ubo_alignment - 1) &
        ~(common.ubo_alignment - 1);
    common.ubo_sync_index_stride = common.ubo_offset;

    if (common.ubo_offset != 0)
        common.ubo = std::unique_ptr<Buffer>(new Buffer(device,
            memory_properties, common.ubo_offset * deferred_calls.size(),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));

    common.ubo_mapped = static_cast<uint8_t*>(common.ubo->map());
    return true;
}

bool vulkan_filter_chain::init_alias()
{
    unsigned i, j;
    common.texture_semantic_map.clear();
    common.texture_semantic_uniform_map.clear();

    for (i = 0; i < passes.size(); i++)
    {
        const std::string name = passes[i]->get_name();
        if (name.empty())
            continue;

        j = &passes[i] - passes.data();

        if (!slang_set_unique_map(
            common.texture_semantic_map, name,
            slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, j }))
            return false;

        if (!slang_set_unique_map(
            common.texture_semantic_uniform_map, name + "Size",
            slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, j }))
            return false;

        if (!slang_set_unique_map(
            common.texture_semantic_map, name + "Feedback",
            slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, j }))
            return false;

        if (!slang_set_unique_map(
            common.texture_semantic_uniform_map, name + "FeedbackSize",
            slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, j }))
            return false;
    }

    for (i = 0; i < common.luts.size(); i++)
    {
        j = &common.luts[i] - common.luts.data();
        if (!slang_set_unique_map(
            common.texture_semantic_map,
            common.luts[i]->get_id(),
            slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, j }))
            return false;

        if (!slang_set_unique_map(
            common.texture_semantic_uniform_map,
            common.luts[i]->get_id() + "Size",
            slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, j }))
            return false;
    }

    return true;
}

bool vulkan_filter_chain::init_feedback()
{
    unsigned i;
    bool use_feedbacks = false;

    common.fb_feedback.clear();

    /* Final pass cannot have feedback. */
    for (i = 0; i < passes.size() - 1; i++)
    {
        bool use_feedback = false;
        for (auto& pass : passes)
        {
            const slang_reflection& r = pass->get_reflection();
            auto& feedbacks = r.semantic_textures[
                SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK];

            if (i < feedbacks.size() && feedbacks[i].texture)
            {
                use_feedback = true;
                use_feedbacks = true;
                break;
            }
        }

        //if (use_feedback && !passes[i]->init_feedback())
        //    return false;

        if (use_feedback)
            RARCH_LOG("[Vulkan filter chain]: Using framebuffer feedback for pass #%u.\n", i);
    }

    if (!use_feedbacks)
    {
#ifdef VULKAN_DEBUG
        RARCH_LOG("[Vulkan filter chain]: Not using framebuffer feedback.\n");
#endif
        return true;
    }

    common.fb_feedback.resize(passes.size() - 1);
    require_clear = true;
    return true;
}

bool vulkan_filter_chain::init_history()
{
    unsigned i;
    size_t required_images = 0;

    original_history.clear();
    common.original_history.clear();

    for (i = 0; i < passes.size(); i++)
        required_images =
        std::max(required_images,
            passes[i]->get_reflection().semantic_textures[
                SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY].size());

    if (required_images < 2)
    {
#ifdef VULKAN_DEBUG
        RARCH_LOG("[Vulkan filter chain]: Not using frame history.\n");
#endif
        return true;
    }

    /* We don't need to store array element #0,
     * since it's aliased with the actual original. */
    required_images--;
    original_history.reserve(required_images);
    common.original_history.resize(required_images);

    for (i = 0; i < required_images; i++)
        original_history.emplace_back(new Framebuffer(device, memory_properties,
            max_input_size, original_format, 1));

#ifdef VULKAN_DEBUG
    RARCH_LOG("[Vulkan filter chain]: Using history of %u frames.\n", unsigned(required_images));
#endif

    /* On first frame, we need to clear the textures to
     * a known state, but we need
     * a command buffer for that, so just defer to first frame.
     */
    require_clear = true;
    return true;
}

bool vulkan_filter_chain::init()
{
    unsigned i;
    Size2D source = max_input_size;

    if (!init_alias())
        return false;

    for (i = 0; i < passes.size(); i++)
    {
#ifdef VULKAN_DEBUG
        const char* name = passes[i]->get_name().c_str();
        RARCH_LOG("[slang]: Building pass #%u (%s)\n", i,
            string_is_empty(name) ?
            msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NOT_AVAILABLE) :
            name);
#endif
        source = passes[i]->set_pass_info(max_input_size,
            source, swapchain_info, pass_info[i]);
        if (!passes[i]->build())
            return false;
    }

    require_clear = false;
    if (!init_ubo())
        return false;
    if (!init_history())
        return false;
    if (!init_feedback())
        return false;
    common.pass_outputs.resize(passes.size());
    return true;
}

bool vulkan_filter_chain::update_swapchain_info(
    const vulkan_filter_chain_swapchain_info& info)
{
    flush();
    set_swapchain_info(info);
    return init();
}

void vulkan_filter_chain::flush()
{
    vkDeviceWaitIdle(device);
    execute_deferred();
}

void vulkan_filter_chain::set_swapchain_info(
    const vulkan_filter_chain_swapchain_info& info)
{
    swapchain_info = info;
    set_num_sync_indices(info.num_indices);
}

void vulkan_filter_chain::execute_deferred()
{
    for (auto& calls : deferred_calls)
    {
        for (auto& call : calls)
            call();
        calls.clear();
    }
}

void vulkan_filter_chain::set_num_sync_indices(unsigned num_indices)
{
    execute_deferred();
    deferred_calls.resize(num_indices);
}

void vulkan_filter_chain::build_offscreen_passes(VkCommandBuffer cmd,
    const VkViewport& vp)
{
    unsigned i;
    Texture source;

    /* First frame, make sure our history and feedback textures
     * are in a clean state. */
    if (require_clear)
    {
        clear_history_and_feedback(cmd);
        require_clear = false;
    }

    update_history_info();
    update_feedback_info();

    DeferredDisposer disposer(deferred_calls[current_sync_index]);
    const Texture original = {
       input_texture,
       passes.front()->get_source_filter(),
       passes.front()->get_mip_filter(),
       passes.front()->get_address_mode(),
    };

    source = original;

    for (i = 0; i < passes.size() - 1; i++)
    {
        passes[i]->build_commands(disposer, cmd,
            original, source, vp, nullptr);

        const Framebuffer& fb = passes[i]->get_framebuffer();

        source.texture.view = fb.get_view();
        source.texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        source.texture.width = fb.get_size().width;
        source.texture.height = fb.get_size().height;
        source.filter = passes[i + 1]->get_source_filter();
        source.mip_filter = passes[i + 1]->get_mip_filter();
        source.address = passes[i + 1]->get_address_mode();

        common.pass_outputs[i] = source;
    }
}

void vulkan_filter_chain::build_viewport_pass(
    VkCommandBuffer cmd, const VkViewport& vp, const float* mvp)
{
    unsigned i;
    Texture source;

    /* First frame, make sure our history and
     * feedback textures are in a clean state. */
    if (require_clear)
    {
        clear_history_and_feedback(cmd);
        require_clear = false;
    }

    DeferredDisposer disposer(deferred_calls[current_sync_index]);
    const Texture original = {
       input_texture,
       passes.front()->get_source_filter(),
       passes.front()->get_mip_filter(),
       passes.front()->get_address_mode(),
    };

    if (passes.size() == 1)
    {
        source = {
           input_texture,
           passes.back()->get_source_filter(),
           passes.back()->get_mip_filter(),
           passes.back()->get_address_mode(),
        };
    }
    else
    {
        const Framebuffer& fb = passes[passes.size() - 2]->get_framebuffer();
        source.texture.view = fb.get_view();
        source.texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        source.texture.width = fb.get_size().width;
        source.texture.height = fb.get_size().height;
        source.filter = passes.back()->get_source_filter();
        source.mip_filter = passes.back()->get_mip_filter();
        source.address = passes.back()->get_address_mode();
    }

    passes.back()->build_commands(disposer, cmd,
        original, source, vp, mvp);

    /* For feedback FBOs, swap current and previous. */
    for (i = 0; i < passes.size(); i++)
        passes[i]->end_frame();
}

void vulkan_filter_chain::end_frame(VkCommandBuffer cmd)
{
    /* If we need to keep old frames, copy it after fragment is complete.
     * TODO: We can improve pipelining by figuring out which
     * pass is the last that reads from
     * the history and dispatch the copy earlier. */
    if (!original_history.empty())
    {
        DeferredDisposer disposer(deferred_calls[current_sync_index]);
        update_history(disposer, cmd);
    }
}

void vulkan_filter_chain::clear_history_and_feedback(VkCommandBuffer cmd)
{
    unsigned i;
    for (i = 0; i < original_history.size(); i++)
        vulkan_framebuffer_clear(original_history[i]->get_image(), cmd);
    for (i = 0; i < passes.size(); i++)
    {
        Framebuffer* fb = passes[i]->get_feedback_framebuffer();
        if (fb)
            vulkan_framebuffer_clear(fb->get_image(), cmd);
    }
}

void vulkan_filter_chain::notify_sync_index(unsigned index)
{
    unsigned i;
    auto& calls = deferred_calls[index];
    for (auto& call : calls)
        call();
    calls.clear();

    current_sync_index = index;

    for (i = 0; i < passes.size(); i++)
        passes[i]->notify_sync_index(index);
}

void vulkan_filter_chain::set_frame_count(uint64_t count)
{
    unsigned i;
    for (i = 0; i < passes.size(); i++)
        passes[i]->set_frame_count(count);
}

void vulkan_filter_chain::set_frame_direction(int32_t direction)
{
    unsigned i;
    for (i = 0; i < passes.size(); i++)
        passes[i]->set_frame_direction(direction);
}

void vulkan_filter_chain::set_input_texture(
    const vulkan_filter_chain_texture& texture)
{
    input_texture = texture;
}

void vulkan_filter_chain::update_feedback_info()
{
    unsigned i;
    if (common.fb_feedback.empty())
        return;

    for (i = 0; i < passes.size() - 1; i++)
    {
        Framebuffer* fb = passes[i]->get_feedback_framebuffer();
        if (!fb)
            continue;

        Texture* source = &common.fb_feedback[i];

        if (!source)
            continue;

        source->texture.image = fb->get_image();
        source->texture.view = fb->get_view();
        source->texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        source->texture.width = fb->get_size().width;
        source->texture.height = fb->get_size().height;
        source->filter = passes[i]->get_source_filter();
        source->mip_filter = passes[i]->get_mip_filter();
        source->address = passes[i]->get_address_mode();
    }
}

void vulkan_filter_chain::update_history_info()
{
    unsigned i = 0;

    for (i = 0; i < original_history.size(); i++)
    {
        Texture* source = (Texture*)&common.original_history[i];

        if (!source)
            continue;

        source->texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        source->texture.view = original_history[i]->get_view();
        source->texture.image = original_history[i]->get_image();
        source->texture.width = original_history[i]->get_size().width;
        source->texture.height = original_history[i]->get_size().height;
        source->filter = passes.front()->get_source_filter();
        source->mip_filter = passes.front()->get_mip_filter();
        source->address = passes.front()->get_address_mode();
    }
}

void vulkan_filter_chain::update_history(DeferredDisposer& disposer,
    VkCommandBuffer cmd)
{
    std::unique_ptr<Framebuffer> tmp;
    VkImageLayout src_layout = input_texture.layout;

    /* Transition input texture to something appropriate. */
    if (input_texture.layout != VK_IMAGE_LAYOUT_GENERAL)
    {
        VULKAN_IMAGE_LAYOUT_TRANSITION_LEVELS(cmd,
            input_texture.image, VK_REMAINING_MIP_LEVELS,
            input_texture.layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            0,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED);

        src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    std::unique_ptr<Framebuffer>& back = original_history.back();
    swap(back, tmp);

    if (input_texture.width != tmp->get_size().width ||
        input_texture.height != tmp->get_size().height ||
        (input_texture.format != VK_FORMAT_UNDEFINED
            && input_texture.format != tmp->get_format()))
        tmp->set_size(disposer, { input_texture.width, input_texture.height }, input_texture.format);

    vulkan_framebuffer_copy(tmp->get_image(), tmp->get_size(),
        cmd, input_texture.image, src_layout);

    /* Transition input texture back. */
    if (input_texture.layout != VK_IMAGE_LAYOUT_GENERAL)
    {
        VULKAN_IMAGE_LAYOUT_TRANSITION_LEVELS(cmd,
            input_texture.image, VK_REMAINING_MIP_LEVELS,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            input_texture.layout,
            0,
            VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED);
    }

    /* Should ring buffer, but we don't have *that* many passes. */
    move_backward(begin(original_history), end(original_history) - 1, end(original_history));
    swap(original_history.front(), tmp);
}

void vulkan_filter_chain::set_pass_info(unsigned pass,
    const vulkan_filter_chain_pass_info& info)
{
    pass_info[pass] = info;
}

void vulkan_filter_chain::set_shader(
    unsigned pass,
    VkShaderStageFlags stage,
    const uint32_t* spirv,
    size_t spirv_words)
{
    passes[pass]->set_shader(stage, spirv, spirv_words);
}

vulkan_filter_chain::vulkan_filter_chain(
    const vulkan_filter_chain_create_info& info)
    : device(info.device),
    gpu(info.gpu),
    memory_properties(*info.memory_properties),
    cache(info.pipeline_cache),
    common(info.device, *info.memory_properties),
    original_format(info.original_format)
{
    max_input_size = { info.max_input_size.width, info.max_input_size.height };
    set_swapchain_info(info.swapchain);
    set_num_passes(info.num_passes);
}

vulkan_filter_chain::~vulkan_filter_chain()
{
    flush();
}

void vulkan_filter_chain::set_num_passes(unsigned num_passes)
{
    unsigned i;

    pass_info.resize(num_passes);
    passes.reserve(num_passes);

    for (i = 0; i < num_passes; i++)
    {
        passes.emplace_back(new Pass(device, memory_properties,
            cache, deferred_calls.size(), i + 1 == num_passes));
        passes.back()->set_common_resources(&common);
        passes.back()->set_pass_number(i);
    }
}

static const uint32_t opaque_vert[] =
#include "vulkan_shaders/opaque.vert.inc"
;

static const uint32_t opaque_frag[] =
#include "vulkan_shaders/opaque.frag.inc"
;

vulkan_filter_chain_t* vulkan_filter_chain_create_default(
    const struct vulkan_filter_chain_create_info* info,
    glslang_filter_chain_filter filter)
{
    struct vulkan_filter_chain_pass_info pass_info;
    auto tmpinfo = *info;

    tmpinfo.num_passes = 1;

    std::unique_ptr<vulkan_filter_chain> chain{ new vulkan_filter_chain(tmpinfo) };
    if (!chain)
        return nullptr;

    pass_info.scale_type_x = GLSLANG_FILTER_CHAIN_SCALE_VIEWPORT;
    pass_info.scale_type_y = GLSLANG_FILTER_CHAIN_SCALE_VIEWPORT;
    pass_info.scale_x = 1.0f;
    pass_info.scale_y = 1.0f;
    pass_info.rt_format = tmpinfo.swapchain.format;
    pass_info.source_filter = filter;
    pass_info.mip_filter = GLSLANG_FILTER_CHAIN_NEAREST;
    pass_info.address = GLSLANG_FILTER_CHAIN_ADDRESS_CLAMP_TO_EDGE;
    pass_info.max_levels = 0;

    chain->set_pass_info(0, pass_info);

    chain->set_shader(0, VK_SHADER_STAGE_VERTEX_BIT,
        opaque_vert,
        sizeof(opaque_vert) / sizeof(uint32_t));
    chain->set_shader(0, VK_SHADER_STAGE_FRAGMENT_BIT,
        opaque_frag,
        sizeof(opaque_frag) / sizeof(uint32_t));

    if (!chain->init())
        return nullptr;

    return chain.release();
}

void vulkan_filter_chain_free(
    vulkan_filter_chain_t* chain)
{
    delete chain;
}

bool vulkan_filter_chain_update_swapchain_info(
    vulkan_filter_chain_t* chain,
    const vulkan_filter_chain_swapchain_info* info)
{
    return chain->update_swapchain_info(*info);
}

void vulkan_filter_chain_notify_sync_index(
    vulkan_filter_chain_t* chain,
    unsigned index)
{
    chain->notify_sync_index(index);
}

void vulkan_filter_chain_set_frame_count(
    vulkan_filter_chain_t* chain,
    uint64_t count)
{
    chain->set_frame_count(count);
}

void vulkan_filter_chain_set_frame_direction(
    vulkan_filter_chain_t* chain,
    int32_t direction)
{
    chain->set_frame_direction(direction);
}

void vulkan_filter_chain_set_input_texture(
    vulkan_filter_chain_t* chain,
    const struct vulkan_filter_chain_texture* texture)
{
    chain->set_input_texture(*texture);
}

void vulkan_filter_chain_build_offscreen_passes(
    vulkan_filter_chain_t* chain,
    VkCommandBuffer cmd, const VkViewport* vp)
{
    chain->build_offscreen_passes(cmd, *vp);
}

void vulkan_filter_chain_build_viewport_pass(
    vulkan_filter_chain_t* chain,
    VkCommandBuffer cmd, const VkViewport* vp, const float* mvp)
{
    chain->build_viewport_pass(cmd, *vp, mvp);
}

void vulkan_filter_chain_end_frame(
    vulkan_filter_chain_t* chain,
    VkCommandBuffer cmd)
{
    chain->end_frame(cmd);
}

struct video_shader* vulkan_filter_chain_get_preset(
    vulkan_filter_chain_t* chain)
{
    return chain->get_shader_preset();
}
