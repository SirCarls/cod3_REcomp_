// Checks the HDR10 output pass (SDK patch 0044, src/ui/shaders/hdr10_encode_ps.frag) on a real
// Vulkan implementation, offscreen: an SDR canvas with known values (R16G16B16A16_SFLOAT, alpha =
// UI coverage) is drawn through the presenter's own vertex shader and the HDR10 shader into an
// A2B10G10R10 image, read back and compared with a double-precision reference of the same math.
//
//   clang++ -std=c++17 -O1 -I <sdk>/thirdparty/vulkan-headers/include -I <sdk>/src/ui/shaders \
//     hdr10_encode_test.cpp -lvulkan -o hdr10_encode_test
//   VK_ICD_FILENAMES=<swiftshader>/vk_swiftshader_icd.json ./hdr10_encode_test
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace shaders {
#include "vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
#include "vulkan_spirv/hdr10_encode_ps.h"
}  // namespace shaders

#define CHECK(x)                                                     \
  do {                                                               \
    VkResult r_ = (x);                                               \
    if (r_ != VK_SUCCESS) {                                          \
      std::printf("FAIL %s = %d (line %d)\n", #x, int(r_), __LINE__); \
      return 2;                                                      \
    }                                                                \
  } while (0)

static uint16_t ToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = x & 0x7FFFFFu;
  if (exp <= 0) return uint16_t(sign);  // the test values have no denormals
  if (exp >= 31) return uint16_t(sign | 0x7C00u);
  // Round to nearest even.
  uint32_t half = sign | (uint32_t(exp) << 10) | (mant >> 13);
  uint32_t rest = mant & 0x1FFFu;
  if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;
  return uint16_t(half);
}

static float FromHalf(uint16_t h) {
  uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  uint32_t x;
  if (!exp) {
    x = sign;  // zero (no denormals here)
  } else {
    x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}

struct Px {
  float r, g, b, a;
  const char* what;
};

// The reference (hdr10_encode_ps.frag in double precision).
static void Reference(const Px& p, double paper, double peak, double knee, double power,
                      double out[3]) {
  double e[3] = {std::clamp<double>(p.r, 0, 1), std::clamp<double>(p.g, 0, 1),
                 std::clamp<double>(p.b, 0, 1)};
  double cov = std::clamp<double>(p.a, 0, 1);
  double l[3];
  for (int i = 0; i < 3; ++i) l[i] = std::pow(e[i], 2.2);
  const double m[3][3] = {{0.627403896, 0.329283039, 0.043313065},
                          {0.069097289, 0.919540395, 0.011362316},
                          {0.016391439, 0.088013308, 0.895595253}};
  double c[3];
  for (int i = 0; i < 3; ++i) c[i] = m[i][0] * l[0] + m[i][1] * l[1] + m[i][2] * l[2];
  double y = 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2];
  double ratio = std::max(peak / std::max(paper, 1.0), 1.0);
  double scale = 1.0;
  if (y > knee && ratio > 1.0) {
    double t = std::clamp((y - knee) / std::max(1.0 - knee, 1e-4), 0.0, 1.0);
    scale = 1.0 + (ratio - 1.0) * std::pow(t, power) / y;
  }
  scale = scale + (1.0 - scale) * cov;
  const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625,
               c3 = 18.6875;
  for (int i = 0; i < 3; ++i) {
    double L = std::clamp(c[i] * scale * paper / 10000.0, 0.0, 1.0);
    double lp = std::pow(L, m1);
    out[i] = std::pow((c1 + c2 * lp) / (1.0 + c3 * lp), m2);
  }
}

static double PqToNits(double v) {
  const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625,
               c3 = 18.6875;
  double vp = std::pow(v, 1.0 / m2);
  return 10000.0 * std::pow(std::max(vp - c1, 0.0) / (c2 - c3 * vp), 1.0 / m1);
}

int main() {
  const float kPaper = 203.0f, kPeak = 600.0f, kKnee = 0.8f, kPower = 3.0f;
  std::vector<Px> px = {
      {0.0f, 0.0f, 0.0f, 0.0f, "black"},
      {0.25f, 0.25f, 0.25f, 0.0f, "gray 0.25"},
      {0.5f, 0.5f, 0.5f, 0.0f, "gray 0.5"},
      {0.75f, 0.75f, 0.75f, 0.0f, "gray 0.75"},
      {0.9f, 0.9f, 0.9f, 0.0f, "gray 0.9 (near the knee)"},
      {0.95f, 0.95f, 0.95f, 0.0f, "gray 0.95 (highlight)"},
      {1.0f, 1.0f, 1.0f, 0.0f, "white (game): up to peak"},
      {1.0f, 1.0f, 1.0f, 1.0f, "white (UI): paper white"},
      {1.0f, 1.0f, 1.0f, 0.5f, "white (half UI)"},
      {1.0f, 0.0f, 0.0f, 0.0f, "red"},
      {0.0f, 1.0f, 0.0f, 0.0f, "green"},
      {0.0f, 0.0f, 1.0f, 0.0f, "blue"},
      {1.0f, 0.8f, 0.2f, 0.0f, "orange"},
      {0.5f, 0.5f, 0.5f, 1.0f, "gray 0.5 (UI)"},
      {1.0f, 1.0f, 1.0f, 2.0f, "white (two UI layers)"},
      {0.3f, 0.6f, 0.9f, 0.0f, "sky blue"},
  };
  const uint32_t W = uint32_t(px.size()), H = 2;

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance;
  CHECK(vkCreateInstance(&ici, nullptr, &instance));
  uint32_t n = 1;
  VkPhysicalDevice pd;
  vkEnumeratePhysicalDevices(instance, &n, &pd);
  if (!n) {
    std::printf("FAIL no Vulkan device\n");
    return 2;
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  std::printf("device: %s\n", props.deviceName);
  uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
  uint32_t qf = 0;
  while (qf < qf_count && !(qfs[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++qf;
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qf;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice dev;
  CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
  VkQueue queue;
  vkGetDeviceQueue(dev, qf, 0, &queue);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  auto mem_type = [&](uint32_t bits, VkMemoryPropertyFlags want) -> uint32_t {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
  };
  auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImage& img,
                        VkDeviceMemory& mem) -> VkResult {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {W, H, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VkResult r = vkCreateImage(dev, &ii, nullptr, &img);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    r = vkAllocateMemory(dev, &ai, nullptr, &mem);
    if (r != VK_SUCCESS) return r;
    return vkBindImageMemory(dev, img, mem, 0);
  };
  auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                         VkDeviceMemory& mem) -> VkResult {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VkResult r = vkCreateBuffer(dev, &bi, nullptr, &buf);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        mem_type(req.memoryTypeBits,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = vkAllocateMemory(dev, &ai, nullptr, &mem);
    if (r != VK_SUCCESS) return r;
    return vkBindBufferMemory(dev, buf, mem, 0);
  };

  const VkFormat kCanvas = VK_FORMAT_R16G16B16A16_SFLOAT;
  const VkFormat kOut = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  VkImage canvas, out;
  VkDeviceMemory canvas_mem, out_mem;
  CHECK(make_image(kCanvas, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, canvas,
                   canvas_mem));
  CHECK(make_image(kOut, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   out, out_mem));
  VkBuffer up, down;
  VkDeviceMemory up_mem, down_mem;
  CHECK(make_buffer(W * H * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, up, up_mem));
  CHECK(make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, down, down_mem));
  {
    uint16_t* p;
    vkMapMemory(dev, up_mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&p));
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        uint16_t* q = p + (y * W + x) * 4;
        q[0] = ToHalf(px[x].r);
        q[1] = ToHalf(px[x].g);
        q[2] = ToHalf(px[x].b);
        q[3] = ToHalf(px[x].a);
      }
    vkUnmapMemory(dev, up_mem);
  }

  VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView canvas_view, out_view;
  vi.image = canvas;
  vi.format = kCanvas;
  CHECK(vkCreateImageView(dev, &vi, nullptr, &canvas_view));
  vi.image = out;
  vi.format = kOut;
  CHECK(vkCreateImageView(dev, &vi, nullptr, &out_view));

  // As the presenter's guest output descriptor set layout: an image and an immutable sampler.
  VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler;
  CHECK(vkCreateSampler(dev, &si, nullptr, &sampler));
  VkDescriptorSetLayoutBinding binds[2] = {};
  binds[0].binding = 0;
  binds[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  binds[0].descriptorCount = 1;
  binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binds[1].binding = 1;
  binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  binds[1].descriptorCount = 1;
  binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binds[1].pImmutableSamplers = &sampler;
  VkDescriptorSetLayoutCreateInfo dsli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsli.bindingCount = 2;
  dsli.pBindings = binds;
  VkDescriptorSetLayout dsl;
  CHECK(vkCreateDescriptorSetLayout(dev, &dsli, nullptr, &dsl));
  VkPushConstantRange ranges[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 16},
                                   {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16}};
  VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &dsl;
  pli.pushConstantRangeCount = 2;
  pli.pPushConstantRanges = ranges;
  VkPipelineLayout layout;
  CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &layout));
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 2;
  dpi.pPoolSizes = sizes;
  VkDescriptorPool pool;
  CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &pool));
  VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &dsl;
  VkDescriptorSet set;
  CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
  VkDescriptorImageInfo dii = {VK_NULL_HANDLE, canvas_view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w.pImageInfo = &dii;
  vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

  VkAttachmentDescription att = {};
  att.format = kOut;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  VkRenderPass rp;
  CHECK(vkCreateRenderPass(dev, &rpi, nullptr, &rp));
  VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbi.renderPass = rp;
  fbi.attachmentCount = 1;
  fbi.pAttachments = &out_view;
  fbi.width = W;
  fbi.height = H;
  fbi.layers = 1;
  VkFramebuffer fb;
  CHECK(vkCreateFramebuffer(dev, &fbi, nullptr, &fb));

  VkShaderModule vs, fs;
  VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smi.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
  smi.pCode = shaders::guest_output_triangle_strip_rect_vs;
  CHECK(vkCreateShaderModule(dev, &smi, nullptr, &vs));
  smi.codeSize = sizeof(shaders::hdr10_encode_ps);
  smi.pCode = shaders::hdr10_encode_ps;
  CHECK(vkCreateShaderModule(dev, &smi, nullptr, &fs));
  VkPipelineShaderStageCreateInfo st[2] = {};
  st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  st[0].module = vs;
  st[0].pName = "main";
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  st[1].module = fs;
  st[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vis = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ias = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = vps.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {};
  cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cbs = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1;
  cbs.pAttachments = &cba;
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpi.stageCount = 2;
  gpi.pStages = st;
  gpi.pVertexInputState = &vis;
  gpi.pInputAssemblyState = &ias;
  gpi.pViewportState = &vps;
  gpi.pRasterizationState = &rs;
  gpi.pMultisampleState = &ms;
  gpi.pColorBlendState = &cbs;
  gpi.pDynamicState = &ds;
  gpi.layout = layout;
  gpi.renderPass = rp;
  gpi.basePipelineIndex = -1;
  VkPipeline pipeline;
  CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));

  VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.queueFamilyIndex = qf;
  VkCommandPool cp;
  CHECK(vkCreateCommandPool(dev, &cpi, nullptr, &cp));
  VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cp;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cb;
  CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
  VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cb, &cbbi);
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = canvas;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy copy = {};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {W, H, 1};
  vkCmdCopyBufferToImage(cb, up, canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
  VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpbi.renderPass = rp;
  rpbi.framebuffer = fb;
  rpbi.renderArea.extent = {W, H};
  vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp = {0, 0, float(W), float(H), 0, 1};
  vkCmdSetViewport(cb, 0, 1, &vp);
  VkRect2D sc = {{0, 0}, {W, H}};
  vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
  const float rect[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
  vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rect);
  const float k[4] = {kPaper, kPeak, kKnee, kPower};
  vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16, k);
  vkCmdDraw(cb, 4, 1, 0, 0);
  vkCmdEndRenderPass(cb);
  VkBufferImageCopy back = {};
  back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  back.imageExtent = {W, H, 1};
  vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, 1, &back);
  vkEndCommandBuffer(cb);
  VkSubmitInfo sub_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  sub_info.commandBufferCount = 1;
  sub_info.pCommandBuffers = &cb;
  CHECK(vkQueueSubmit(queue, 1, &sub_info, VK_NULL_HANDLE));
  CHECK(vkQueueWaitIdle(queue));

  uint32_t* res;
  vkMapMemory(dev, down_mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&res));
  int bad = 0;
  std::printf("%-28s %8s %8s %8s | %9s %9s %9s | max err (10-bit codes)\n", "canvas", "R", "G",
              "B", "nits R", "nits G", "nits B");
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      uint32_t v = res[y * W + x];
      double got[3] = {(v & 1023) / 1023.0, ((v >> 10) & 1023) / 1023.0,
                       ((v >> 20) & 1023) / 1023.0};
      // The shader sees the half-float canvas values.
      Px in = px[x];
      in.r = FromHalf(ToHalf(in.r));
      in.g = FromHalf(ToHalf(in.g));
      in.b = FromHalf(ToHalf(in.b));
      in.a = FromHalf(ToHalf(in.a));
      double want[3];
      Reference(in, kPaper, kPeak, kKnee, kPower, want);
      double err = 0;
      for (int i = 0; i < 3; ++i) err = std::max(err, std::fabs(got[i] - want[i]) * 1023.0);
      if (err > 1.5) ++bad;
      if (y == 0)
        std::printf("%-28s %8.4f %8.4f %8.4f | %9.2f %9.2f %9.2f | %.2f%s\n", px[x].what, got[0],
                    got[1], got[2], PqToNits(got[0]), PqToNits(got[1]), PqToNits(got[2]), err,
                    err > 1.5 ? "  <-- MISMATCH" : "");
    }
  std::printf("%s: %d of %u pixels outside 1.5 codes of the reference\n", bad ? "FAIL" : "PASS",
              bad, W * H);
  return bad ? 1 : 0;
}
