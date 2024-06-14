/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_framebuffer.hh"
#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_memory.hh"
#include "vk_state_manager.hh"
#include "vk_texture.hh"

namespace blender::gpu {

/**
 * The default load store action when not using load stores.
 */
constexpr GPULoadStore default_load_store()
{
  return {GPU_LOADACTION_LOAD, GPU_STOREACTION_STORE, {0.0f, 0.0f, 0.0f, 0.0f}};
}

/* -------------------------------------------------------------------- */
/** \name Creation & Deletion
 * \{ */

VKFrameBuffer::VKFrameBuffer(const char *name)
    : FrameBuffer(name),
      load_stores(GPU_FB_MAX_ATTACHMENT, default_load_store()),
      attachment_states_(GPU_FB_MAX_ATTACHMENT, GPU_ATTACHMENT_WRITE)
{
  size_set(1, 1);
  srgb_ = false;
  enabled_srgb_ = false;
}

VKFrameBuffer::~VKFrameBuffer()
{
  render_pass_free();
}

/** \} */

void VKFrameBuffer::bind(bool enabled_srgb)
{
  VKContext &context = *VKContext::get();
  /* Updating attachments can issue pipeline barriers, this should be done outside the render pass.
   * When done inside a render pass there should be a self-dependency between sub-passes on the
   * active render pass. As the active render pass isn't aware of the new render pass (and should
   * not) it is better to deactivate it before updating the attachments. For more information check
   * `VkSubpassDependency`. */
  if (context.has_active_framebuffer()) {
    context.deactivate_framebuffer();
  }

  context.activate_framebuffer(*this);
  enabled_srgb_ = enabled_srgb;
  Shader::set_framebuffer_srgb_target(enabled_srgb && srgb_);
  load_stores.fill(default_load_store());
  attachment_states_.fill(GPU_ATTACHMENT_WRITE);
}

Array<VkViewport, 16> VKFrameBuffer::vk_viewports_get() const
{
  Array<VkViewport, 16> viewports(this->multi_viewport_ ? GPU_MAX_VIEWPORTS : 1);

  int index = 0;
  for (VkViewport &viewport : viewports) {
    viewport.x = viewport_[index][0];
    viewport.y = viewport_[index][1];
    viewport.width = viewport_[index][2];
    viewport.height = viewport_[index][3];
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    index++;
  }
  return viewports;
}

Array<VkRect2D, 16> VKFrameBuffer::vk_render_areas_get() const
{
  Array<VkRect2D, 16> render_areas(this->multi_viewport_ ? GPU_MAX_VIEWPORTS : 1);

  for (VkRect2D &render_area : render_areas) {
    if (scissor_test_get()) {
      int scissor_rect[4];
      scissor_get(scissor_rect);
      render_area.offset.x = clamp_i(scissor_rect[0], 0, width_);
      render_area.offset.y = clamp_i(scissor_rect[1], 0, height_);
      render_area.extent.width = clamp_i(scissor_rect[2], 1, width_ - scissor_rect[0]);
      render_area.extent.height = clamp_i(scissor_rect[3], 1, height_ - scissor_rect[1]);
    }
    else {
      render_area.offset.x = 0;
      render_area.offset.y = 0;
      render_area.extent.width = width_;
      render_area.extent.height = height_;
    }
  }
  return render_areas;
}

bool VKFrameBuffer::check(char /*err_out*/[256])
{
  return true;
}

void VKFrameBuffer::build_clear_attachments_depth_stencil(
    const eGPUFrameBufferBits buffers,
    float clear_depth,
    uint32_t clear_stencil,
    render_graph::VKClearAttachmentsNode::CreateInfo &clear_attachments) const
{
  VkImageAspectFlags aspect_mask = (buffers & GPU_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                   (buffers & GPU_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);

  VkClearAttachment &clear_attachment =
      clear_attachments.attachments[clear_attachments.attachment_count++];
  clear_attachment.aspectMask = aspect_mask;
  clear_attachment.clearValue.depthStencil.depth = clear_depth;
  clear_attachment.clearValue.depthStencil.stencil = clear_stencil;
  clear_attachment.colorAttachment = 0;
}

void VKFrameBuffer::build_clear_attachments_color(
    const float (*clear_colors)[4],
    const bool multi_clear_colors,
    render_graph::VKClearAttachmentsNode::CreateInfo &clear_attachments) const
{
  int color_index = 0;
  for (int color_slot = 0; color_slot < GPU_FB_MAX_COLOR_ATTACHMENT; color_slot++) {
    const GPUAttachment &attachment = attachments_[GPU_FB_COLOR_ATTACHMENT0 + color_slot];
    if (attachment.tex == nullptr) {
      continue;
    }
    VkClearAttachment &clear_attachment =
        clear_attachments.attachments[clear_attachments.attachment_count++];
    clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_attachment.colorAttachment = color_slot;
    eGPUDataFormat data_format = to_data_format(GPU_texture_format(attachment.tex));
    clear_attachment.clearValue.color = to_vk_clear_color_value(data_format,
                                                                &clear_colors[color_index]);

    color_index += multi_clear_colors ? 1 : 0;
  }
}

/* -------------------------------------------------------------------- */
/** \name Clear
 * \{ */

void VKFrameBuffer::clear(render_graph::VKClearAttachmentsNode::CreateInfo &clear_attachments)
{
  VKContext &context = *VKContext::get();
  rendering_ensure(context);
  context.render_graph.add_node(clear_attachments);
}

void VKFrameBuffer::clear(const eGPUFrameBufferBits buffers,
                          const float clear_color[4],
                          float clear_depth,
                          uint clear_stencil)
{
  render_graph::VKClearAttachmentsNode::CreateInfo clear_attachments = {};
  clear_attachments.vk_clear_rect.rect = vk_render_areas_get()[0];
  clear_attachments.vk_clear_rect.baseArrayLayer = 0;
  clear_attachments.vk_clear_rect.layerCount = 1;

  if (buffers & (GPU_DEPTH_BIT | GPU_STENCIL_BIT)) {
    VKContext &context = *VKContext::get();
    eGPUWriteMask needed_mask = GPU_WRITE_NONE;
    if (buffers & GPU_DEPTH_BIT) {
      needed_mask |= GPU_WRITE_DEPTH;
    }
    if (buffers & GPU_STENCIL_BIT) {
      needed_mask |= GPU_WRITE_STENCIL;
    }

    /* Clearing depth via vkCmdClearAttachments requires a render pass with write depth or stencil
     * enabled. When not enabled, clearing should be done via texture directly. */
    if ((context.state_manager_get().state.write_mask & needed_mask) == needed_mask) {
      build_clear_attachments_depth_stencil(
          buffers, clear_depth, clear_stencil, clear_attachments);
    }
    else {
      VKTexture *depth_texture = unwrap(unwrap(depth_tex()));
      if (depth_texture != nullptr) {
        if (G.debug & G_DEBUG_GPU) {
          std::cout
              << "PERFORMANCE: impact clearing depth texture in render pass that doesn't allow "
                 "depth writes.\n";
        }
        depth_attachment_layout_ensure(context, VK_IMAGE_LAYOUT_GENERAL);
        depth_texture->clear_depth_stencil(buffers, clear_depth, clear_stencil);
      }
    }
  }
  if (buffers & GPU_COLOR_BIT) {
    float clear_color_single[4];
    copy_v4_v4(clear_color_single, clear_color);
    build_clear_attachments_color(&clear_color_single, false, clear_attachments);
  }

  if (clear_attachments.attachment_count) {
    clear(clear_attachments);
  }
}

void VKFrameBuffer::clear_multi(const float (*clear_color)[4])
{
  render_graph::VKClearAttachmentsNode::CreateInfo clear_attachments = {};
  clear_attachments.vk_clear_rect.rect = vk_render_areas_get()[0];
  clear_attachments.vk_clear_rect.baseArrayLayer = 0;
  clear_attachments.vk_clear_rect.layerCount = 1;

  build_clear_attachments_color(clear_color, true, clear_attachments);
  if (clear_attachments.attachment_count) {
    clear(clear_attachments);
  }
}

void VKFrameBuffer::clear_attachment(GPUAttachmentType /*type*/,
                                     eGPUDataFormat /*data_format*/,
                                     const void * /*clear_value*/)
{
  /* Clearing of a single attachment was added to implement `clear_multi` in OpenGL. As
   * `clear_multi` is supported in Vulkan it isn't needed to implement this method.
   */
  BLI_assert_unreachable();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Load/Store operations
 * \{ */

void VKFrameBuffer::attachment_set_loadstore_op(GPUAttachmentType type, GPULoadStore ls)
{
  load_stores[type] = ls;
}

static VkAttachmentLoadOp to_vk_attachment_load_op(eGPULoadOp load_op)
{
  switch (load_op) {
    case GPU_LOADACTION_DONT_CARE:
      return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    case GPU_LOADACTION_CLEAR:
      return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case GPU_LOADACTION_LOAD:
      return VK_ATTACHMENT_LOAD_OP_LOAD;
  }
  BLI_assert_unreachable();
  return VK_ATTACHMENT_LOAD_OP_LOAD;
}

static VkAttachmentStoreOp to_vk_attachment_store_op(eGPUStoreOp store_op)
{
  switch (store_op) {
    case GPU_STOREACTION_DONT_CARE:
      return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case GPU_STOREACTION_STORE:
      return VK_ATTACHMENT_STORE_OP_STORE;
  }
  BLI_assert_unreachable();
  return VK_ATTACHMENT_STORE_OP_STORE;
}

static void set_load_store(VkRenderingAttachmentInfo &r_rendering_attachment,
                           const GPULoadStore &ls)
{
  copy_v4_v4(r_rendering_attachment.clearValue.color.float32, ls.clear_value);
  r_rendering_attachment.loadOp = to_vk_attachment_load_op(ls.load_action);
  r_rendering_attachment.storeOp = to_vk_attachment_store_op(ls.store_action);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sub-pass transition
 * \{ */

void VKFrameBuffer::subpass_transition_impl(const GPUAttachmentState depth_attachment_state,
                                            Span<GPUAttachmentState> color_attachment_states)
{
  // TODO: this is a fallback implementation. We should also provide support for
  // `VK_EXT_dynamic_rendering_local_read`. This extension is only supported on Windows
  // platforms (2024Q2), but would reduce the rendering synchronization overhead.
  VKContext &context = *VKContext::get();
  if (is_rendering_) {
    rendering_end(context);

    // TODO: this might need a better implementation:
    // READ -> DONTCARE
    // WRITE -> LOAD, STORE based on previous value.
    // IGNORE -> DONTCARE -> IGNORE
    load_stores.fill(default_load_store());
  }

  attachment_states_[GPU_FB_DEPTH_ATTACHMENT] = depth_attachment_state;
  attachment_states_.as_mutable_span()
      .slice(GPU_FB_COLOR_ATTACHMENT0, color_attachment_states.size())
      .copy_from(color_attachment_states);
  for (int index : IndexRange(color_attachment_states.size())) {
    if (color_attachment_states[index] == GPU_ATTACHMENT_READ) {
      VKTexture *texture = unwrap(unwrap(color_tex(index)));
      if (texture) {
        context.state_manager_get().texture_bind(
            texture, GPUSamplerState::default_sampler(), index);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Read back
 * \{ */

void VKFrameBuffer::read(eGPUFrameBufferBits plane,
                         eGPUDataFormat format,
                         const int area[4],
                         int /*channel_len*/,
                         int slot,
                         void *r_data)
{
  VKContext &context = *VKContext::get();
  GPUAttachment *attachment = nullptr;
  switch (plane) {
    case GPU_COLOR_BIT:
      attachment = &attachments_[GPU_FB_COLOR_ATTACHMENT0 + slot];
      color_attachment_layout_ensure(context, slot, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      break;

    case GPU_DEPTH_BIT:
      attachment = attachments_[GPU_FB_DEPTH_ATTACHMENT].tex ?
                       &attachments_[GPU_FB_DEPTH_ATTACHMENT] :
                       &attachments_[GPU_FB_DEPTH_STENCIL_ATTACHMENT];
      depth_attachment_layout_ensure(context, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      break;

    default:
      BLI_assert_unreachable();
      return;
  }

  VKTexture *texture = unwrap(unwrap(attachment->tex));
  BLI_assert_msg(texture,
                 "Trying to read back texture from framebuffer, but no texture is available in "
                 "requested slot.");
  if (texture == nullptr) {
    return;
  }
  const int area6[6] = {area[0], area[1], 0, area[2], area[3], 1};
  IndexRange layers(max_ii(attachment->layer, 0), 1);
  texture->read_sub(0, format, area6, layers, r_data);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blit operations
 * \{ */

static void blit_aspect(VKContext &context,
                        VKTexture &dst_texture,
                        VKTexture &src_texture,
                        int dst_offset_x,
                        int dst_offset_y,
                        VkImageAspectFlags image_aspect)
{
  /* Prefer texture copy, as some platforms don't support using D32_SFLOAT_S8_UINT to be used as
   * a blit destination. */
  if (dst_offset_x == 0 && dst_offset_y == 0 &&
      dst_texture.device_format_get() == src_texture.device_format_get() &&
      src_texture.width_get() == dst_texture.width_get() &&
      src_texture.height_get() == dst_texture.height_get())
  {
    src_texture.copy_to(dst_texture, image_aspect);
    return;
  }

  render_graph::VKBlitImageNode::CreateInfo blit_image = {};

  blit_image.src_image = src_texture.vk_image_handle();
  blit_image.dst_image = dst_texture.vk_image_handle();
  blit_image.filter = VK_FILTER_NEAREST;

  VkImageBlit &region = blit_image.region;
  region.srcSubresource.aspectMask = image_aspect;
  region.srcSubresource.mipLevel = 0;
  region.srcSubresource.baseArrayLayer = 0;
  region.srcSubresource.layerCount = 1;
  region.srcOffsets[0].x = 0;
  region.srcOffsets[0].y = 0;
  region.srcOffsets[0].z = 0;
  region.srcOffsets[1].x = src_texture.width_get();
  region.srcOffsets[1].y = src_texture.height_get();
  region.srcOffsets[1].z = 1;

  region.dstSubresource.aspectMask = image_aspect;
  region.dstSubresource.mipLevel = 0;
  region.dstSubresource.baseArrayLayer = 0;
  region.dstSubresource.layerCount = 1;
  region.dstOffsets[0].x = min_ii(dst_offset_x, dst_texture.width_get());
  region.dstOffsets[0].y = min_ii(dst_offset_y, dst_texture.height_get());
  region.dstOffsets[0].z = 0;
  region.dstOffsets[1].x = min_ii(dst_offset_x + src_texture.width_get(), dst_texture.width_get());
  region.dstOffsets[1].y = min_ii(dst_offset_y + src_texture.height_get(),
                                  dst_texture.height_get());
  region.dstOffsets[1].z = 1;

  context.render_graph.add_node(blit_image);
}

void VKFrameBuffer::blit_to(eGPUFrameBufferBits planes,
                            int src_slot,
                            FrameBuffer *dst,
                            int dst_slot,
                            int dst_offset_x,
                            int dst_offset_y)
{
  BLI_assert(dst);
  BLI_assert_msg(ELEM(planes, GPU_COLOR_BIT, GPU_DEPTH_BIT),
                 "VKFrameBuffer::blit_to only supports a single color or depth aspect.");
  UNUSED_VARS_NDEBUG(planes);

  VKContext &context = *VKContext::get();
  if (!context.has_active_framebuffer()) {
    BLI_assert_unreachable();
    return;
  }

  VKFrameBuffer &dst_framebuffer = *unwrap(dst);
  if (planes & GPU_COLOR_BIT) {
    const GPUAttachment &src_attachment = attachments_[GPU_FB_COLOR_ATTACHMENT0 + src_slot];
    const GPUAttachment &dst_attachment =
        dst_framebuffer.attachments_[GPU_FB_COLOR_ATTACHMENT0 + dst_slot];
    if (src_attachment.tex && dst_attachment.tex) {
      VKTexture &src_texture = *unwrap(unwrap(src_attachment.tex));
      VKTexture &dst_texture = *unwrap(unwrap(dst_attachment.tex));
      blit_aspect(context,
                  dst_texture,
                  src_texture,
                  dst_offset_x,
                  dst_offset_y,
                  VK_IMAGE_ASPECT_COLOR_BIT);
    }
  }

  if (planes & GPU_DEPTH_BIT) {
    /* Retrieve source texture. */
    const GPUAttachment &src_attachment = attachments_[GPU_FB_DEPTH_STENCIL_ATTACHMENT].tex ?
                                              attachments_[GPU_FB_DEPTH_STENCIL_ATTACHMENT] :
                                              attachments_[GPU_FB_DEPTH_ATTACHMENT];
    const GPUAttachment &dst_attachment =
        dst_framebuffer.attachments_[GPU_FB_DEPTH_STENCIL_ATTACHMENT].tex ?
            dst_framebuffer.attachments_[GPU_FB_DEPTH_STENCIL_ATTACHMENT] :
            dst_framebuffer.attachments_[GPU_FB_DEPTH_ATTACHMENT];
    if (src_attachment.tex && dst_attachment.tex) {
      VKTexture &src_texture = *unwrap(unwrap(src_attachment.tex));
      VKTexture &dst_texture = *unwrap(unwrap(dst_attachment.tex));
      blit_aspect(context,
                  dst_texture,
                  src_texture,
                  dst_offset_x,
                  dst_offset_y,
                  VK_IMAGE_ASPECT_DEPTH_BIT);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Update attachments
 * \{ */

void VKFrameBuffer::vk_render_pass_ensure()
{
  if (!dirty_attachments_) {
    return;
  }
  render_pass_free();
  render_pass_create();

  dirty_attachments_ = false;
}

void VKFrameBuffer::render_pass_create()
{
  BLI_assert(vk_render_pass_ == VK_NULL_HANDLE);
  BLI_assert(vk_framebuffer_ == VK_NULL_HANDLE);

  VK_ALLOCATION_CALLBACKS

  /* Track first attachment for size. */
  GPUAttachmentType first_attachment = GPU_FB_MAX_ATTACHMENT;

  std::array<VkAttachmentDescription, GPU_FB_MAX_ATTACHMENT> attachment_descriptions;
  std::array<VkImageView, GPU_FB_MAX_ATTACHMENT> image_views;
  std::array<VkAttachmentReference, GPU_FB_MAX_ATTACHMENT> attachment_references;

  bool has_depth_attachment = false;
  bool found_attachment = false;
  const VKImageView &dummy_attachment =
      VKBackend::get().device_get().dummy_color_attachment_get().image_view_get();
  int depth_location = -1;

  for (int type = GPU_FB_MAX_ATTACHMENT - 1; type >= 0; type--) {
    GPUAttachment &attachment = attachments_[type];
    if (attachment.tex == nullptr && !found_attachment) {
      /* Move the depth texture to the next binding point after all color textures. The binding
       * location of the color textures should be kept in sync between ShaderCreateInfos and the
       * framebuffer attachments. The depth buffer should be the last slot. */
      depth_location = max_ii(type - GPU_FB_COLOR_ATTACHMENT0, 0);
      continue;
    }
    found_attachment |= attachment.tex != nullptr;

    /* Keep the first attachment to the first color attachment, or to the depth buffer when there
     * is no color attachment. */
    if (attachment.tex != nullptr &&
        (first_attachment == GPU_FB_MAX_ATTACHMENT || type >= GPU_FB_COLOR_ATTACHMENT0))
    {
      first_attachment = static_cast<GPUAttachmentType>(type);
    }

    int attachment_location = type >= GPU_FB_COLOR_ATTACHMENT0 ? type - GPU_FB_COLOR_ATTACHMENT0 :
                                                                 depth_location;
    const bool is_depth_attachment = ELEM(
        type, GPU_FB_DEPTH_ATTACHMENT, GPU_FB_DEPTH_STENCIL_ATTACHMENT);

    if (attachment.tex) {
      BLI_assert_msg(!is_depth_attachment || !has_depth_attachment,
                     "There can only be one depth/stencil attachment.");
      has_depth_attachment |= is_depth_attachment;

      /* Ensure texture is allocated to ensure the image view. */
      VKTexture &texture = *static_cast<VKTexture *>(unwrap(attachment.tex));
      VKImageViewInfo image_view_info = {eImageViewUsage::Attachment,
                                         IndexRange(max_ii(attachment.layer, 0), 1),
                                         IndexRange(attachment.mip, 1),
                                         {{'r', 'g', 'b', 'a'}},
                                         false,
                                         srgb_ && enabled_srgb_};
      const VKImageView &image_view = texture.image_view_get(image_view_info);
      image_views[attachment_location] = image_view.vk_handle();

      VkAttachmentDescription &attachment_description =
          attachment_descriptions[attachment_location];
      attachment_description.flags = 0;
      attachment_description.format = image_view.vk_format();
      attachment_description.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment_description.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment_description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment_description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment_description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment_description.initialLayout = texture.current_layout_get();
      attachment_description.finalLayout = texture.current_layout_get();

      /* Create the attachment reference. */
      VkAttachmentReference &attachment_reference = attachment_references[attachment_location];
      attachment_reference.attachment = attachment_location;
      attachment_reference.layout = is_depth_attachment ?
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                                        VK_IMAGE_LAYOUT_GENERAL;
    }
    else if (!is_depth_attachment) {
      image_views[attachment_location] = dummy_attachment.vk_handle();

      VkAttachmentDescription &attachment_description =
          attachment_descriptions[attachment_location];
      attachment_description.flags = 0;
      attachment_description.format = VK_FORMAT_R32_SFLOAT;
      attachment_description.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment_description.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment_description.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment_description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment_description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment_description.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment_description.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

      VkAttachmentReference &attachment_reference = attachment_references[attachment_location];
      attachment_reference.attachment = VK_ATTACHMENT_UNUSED;
      attachment_reference.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }

  /* Update the size, viewport & scissor based on the first attachment. */
  if (first_attachment != GPU_FB_MAX_ATTACHMENT) {
    GPUAttachment &attachment = attachments_[first_attachment];
    BLI_assert(attachment.tex);

    int size[3];
    GPU_texture_get_mipmap_size(attachment.tex, attachment.mip, size);
    size_set(size[0], size[1]);
  }
  else {
    /* A frame-buffer should at least be 1 by 1. */
    this->size_set(1, 1);
  }
  viewport_reset();
  scissor_reset();

  /* Create render pass. */
  const int attachment_len = has_depth_attachment ? depth_location + 1 : depth_location;
  const int color_attachment_len = depth_location;
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = color_attachment_len;
  subpass.pColorAttachments = attachment_references.data();
  if (has_depth_attachment) {
    subpass.pDepthStencilAttachment = &attachment_references[depth_location];
  }

  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = attachment_len;
  render_pass_info.pAttachments = attachment_descriptions.data();
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;

  const VKDevice &device = VKBackend::get().device_get();
  vkCreateRenderPass(
      device.device_get(), &render_pass_info, vk_allocation_callbacks, &vk_render_pass_);

  /* We might want to split frame-buffer and render pass. */
  VkFramebufferCreateInfo framebuffer_create_info = {};
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.renderPass = vk_render_pass_;
  framebuffer_create_info.attachmentCount = attachment_len;
  framebuffer_create_info.pAttachments = image_views.data();
  framebuffer_create_info.width = width_;
  framebuffer_create_info.height = height_;
  framebuffer_create_info.layers = 1;

  vkCreateFramebuffer(
      device.device_get(), &framebuffer_create_info, vk_allocation_callbacks, &vk_framebuffer_);
}

void VKFrameBuffer::render_pass_free()
{
  if (vk_render_pass_ == VK_NULL_HANDLE) {
    return;
  }

  VKDevice &device = VKBackend::get().device_get();
  if (device.is_initialized()) {
    device.discard_render_pass(vk_render_pass_);
    device.discard_frame_buffer(vk_framebuffer_);
  }
  vk_render_pass_ = VK_NULL_HANDLE;
  vk_framebuffer_ = VK_NULL_HANDLE;
}

void VKFrameBuffer::color_attachment_layout_ensure(VKContext &context,
                                                   int color_attachment,
                                                   VkImageLayout requested_layout)
{
  VKTexture *color_texture = unwrap(unwrap(color_tex(color_attachment)));
  if (color_texture == nullptr) {
    return;
  }

  if (color_texture->current_layout_get() == requested_layout) {
    return;
  }

  color_texture->layout_ensure(context, requested_layout);
  dirty_attachments_ = true;
}

void VKFrameBuffer::depth_attachment_layout_ensure(VKContext &context,
                                                   VkImageLayout requested_layout)
{
  VKTexture *depth_texture = unwrap(unwrap(depth_tex()));
  if (depth_texture == nullptr) {
    return;
  }

  if (depth_texture->current_layout_get() == requested_layout) {
    return;
  }
  depth_texture->layout_ensure(context, requested_layout);
  dirty_attachments_ = true;
}

void VKFrameBuffer::update_size()
{
  if (!dirty_attachments_) {
    return;
  }

  for (int i = 0; i < GPU_FB_MAX_ATTACHMENT; i++) {
    GPUAttachment &attachment = attachments_[i];
    if (attachment.tex) {
      int size[3];
      GPU_texture_get_mipmap_size(attachment.tex, attachment.mip, size);
      size_set(size[0], size[1]);
      return;
    }
  }
  size_set(1, 1);
}

void VKFrameBuffer::update_srgb()
{
  for (int i : IndexRange(GPU_FB_MAX_COLOR_ATTACHMENT)) {
    VKTexture *texture = unwrap(unwrap(color_tex(i)));
    if (texture) {
      srgb_ = (texture->format_flag_get() & GPU_FORMAT_SRGB) != 0;
      return;
    }
  }
}

int VKFrameBuffer::color_attachments_resource_size() const
{
  int size = 0;
  for (int color_slot : IndexRange(GPU_FB_MAX_COLOR_ATTACHMENT)) {
    if (color_tex(color_slot) != nullptr) {
      size = max_ii(color_slot + 1, size);
    }
  }
  return size;
}

/** \} */

void VKFrameBuffer::rendering_reset()
{
  is_rendering_ = false;
}

void VKFrameBuffer::rendering_ensure(VKContext &context)
{
  if (is_rendering_) {
    return;
  }
  is_rendering_ = true;
  dirty_attachments_ = false;
  dirty_state_ = false;
  depth_attachment_format_ = VK_FORMAT_UNDEFINED;
  stencil_attachment_format_ = VK_FORMAT_UNDEFINED;

  viewport_reset();
  scissor_reset();

  render_graph::VKResourceAccessInfo access_info;
  render_graph::VKBeginRenderingNode::CreateInfo begin_rendering(access_info);
  begin_rendering.node_data.vk_rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  begin_rendering.node_data.vk_rendering_info.layerCount = 1;
  begin_rendering.node_data.vk_rendering_info.renderArea = vk_render_areas_get()[0];

  color_attachment_formats_.clear();
  for (int color_attachment_index :
       IndexRange(GPU_FB_COLOR_ATTACHMENT0, GPU_FB_MAX_COLOR_ATTACHMENT))
  {
    const GPUAttachment &attachment = attachments_[color_attachment_index];
    if (attachment.tex == nullptr) {
      continue;
    }

    VKTexture &color_texture = *unwrap(unwrap(attachment.tex));
    VkRenderingAttachmentInfo &attachment_info =
        begin_rendering.node_data
            .color_attachments[begin_rendering.node_data.vk_rendering_info.colorAttachmentCount++];
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;

    VkImageView vk_image_view = VK_NULL_HANDLE;
    GPUAttachmentState attachment_state = attachment_states_[color_attachment_index];
    if (attachment_state == GPU_ATTACHMENT_WRITE) {
      VKImageViewInfo image_view_info = {eImageViewUsage::Attachment,
                                         IndexRange(max_ii(attachment.layer, 0), 1),
                                         IndexRange(attachment.mip, 1),
                                         {{'r', 'g', 'b', 'a'}},
                                         false,
                                         srgb_ && enabled_srgb_};
      vk_image_view = color_texture.image_view_get(image_view_info).vk_handle();
    }
    attachment_info.imageView = vk_image_view;
    attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    set_load_store(attachment_info, load_stores[color_attachment_index]);

    access_info.images.append(
        {color_texture.vk_image_handle(),
         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         VK_IMAGE_ASPECT_COLOR_BIT});
    color_attachment_formats_.append(to_vk_format(color_texture.device_format_get()));

    begin_rendering.node_data.vk_rendering_info.pColorAttachments =
        begin_rendering.node_data.color_attachments;
  }

  for (int depth_attachment_index : IndexRange(GPU_FB_DEPTH_ATTACHMENT, 2)) {
    const GPUAttachment &attachment = attachments_[depth_attachment_index];

    if (attachment.tex == nullptr) {
      continue;
    }
    bool is_stencil_attachment = depth_attachment_index == GPU_FB_DEPTH_STENCIL_ATTACHMENT;
    VKTexture &depth_texture = *unwrap(unwrap(attachment.tex));
    bool is_depth_stencil_attachment = to_vk_image_aspect_flag_bits(
                                           depth_texture.device_format_get()) &
                                       VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageLayout vk_image_layout = is_depth_stencil_attachment ?
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkFormat vk_format = to_vk_format(depth_texture.device_format_get());
    GPUAttachmentState attachment_state = attachment_states_[GPU_FB_DEPTH_ATTACHMENT];
    VkImageView depth_image_view = VK_NULL_HANDLE;
    if (attachment_state == GPU_ATTACHMENT_WRITE) {
      VKImageViewInfo image_view_info = {eImageViewUsage::Attachment,
                                         IndexRange(max_ii(attachment.layer, 0), 1),
                                         IndexRange(attachment.mip, 1),
                                         {{'r', 'g', 'b', 'a'}},
                                         is_stencil_attachment,
                                         false};
      depth_image_view = depth_texture.image_view_get(image_view_info).vk_handle();
    }

    // TODO: we should be able to use a single attachment info and only set the
    // pDepthAttachment/pStencilAttachment to the same struct. But perhaps the stencil clear op
    // might be different.
    {
      VkRenderingAttachmentInfo &attachment_info = begin_rendering.node_data.depth_attachment;
      attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      attachment_info.imageView = depth_image_view;
      attachment_info.imageLayout = vk_image_layout;

      set_load_store(attachment_info, load_stores[depth_attachment_index]);
      depth_attachment_format_ = vk_format;
      begin_rendering.node_data.vk_rendering_info.pDepthAttachment =
          &begin_rendering.node_data.depth_attachment;
    }

    if (is_stencil_attachment) {
      VkRenderingAttachmentInfo &attachment_info = begin_rendering.node_data.stencil_attachment;
      attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      attachment_info.imageView = depth_image_view;
      attachment_info.imageLayout = vk_image_layout;

      set_load_store(attachment_info, load_stores[depth_attachment_index]);
      stencil_attachment_format_ = vk_format;
      begin_rendering.node_data.vk_rendering_info.pStencilAttachment =
          &begin_rendering.node_data.stencil_attachment;
    }

    access_info.images.append({depth_texture.vk_image_handle(),
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                               is_stencil_attachment ?
                                   static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                                   VK_IMAGE_ASPECT_STENCIL_BIT) :
                                   static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)});
    break;
  }

  context.render_graph.add_node(begin_rendering);
}

VkFormat VKFrameBuffer::depth_attachment_format_get() const
{
  return depth_attachment_format_;
}
VkFormat VKFrameBuffer::stencil_attachment_format_get() const
{
  return stencil_attachment_format_;
};
Span<VkFormat> VKFrameBuffer::color_attachment_formats_get() const
{
  return color_attachment_formats_;
}

void VKFrameBuffer::rendering_end(VKContext &context)
{
  if (!is_rendering_ && use_explicit_load_store_) {
    rendering_ensure(context);
  }

  if (is_rendering_) {
    render_graph::VKEndRenderingNode::CreateInfo end_rendering = {};
    context.render_graph.add_node(end_rendering);
    is_rendering_ = false;
  }
}

}  // namespace blender::gpu
