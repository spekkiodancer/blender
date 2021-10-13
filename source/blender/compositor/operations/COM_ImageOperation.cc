/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2011, Blender Foundation.
 */

#include "COM_ImageOperation.h"

#include "BKE_scene.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

namespace blender::compositor {

BaseImageOperation::BaseImageOperation()
{
  m_image = nullptr;
  m_buffer = nullptr;
  m_imageFloatBuffer = nullptr;
  m_imageByteBuffer = nullptr;
  m_imageUser = nullptr;
  m_imagewidth = 0;
  m_imageheight = 0;
  m_framenumber = 0;
  m_depthBuffer = nullptr;
  depth_buffer_ = nullptr;
  m_numberOfChannels = 0;
  m_rd = nullptr;
  m_viewName = nullptr;
}
ImageOperation::ImageOperation() : BaseImageOperation()
{
  this->addOutputSocket(DataType::Color);
}
ImageAlphaOperation::ImageAlphaOperation() : BaseImageOperation()
{
  this->addOutputSocket(DataType::Value);
}
ImageDepthOperation::ImageDepthOperation() : BaseImageOperation()
{
  this->addOutputSocket(DataType::Value);
}

ImBuf *BaseImageOperation::getImBuf()
{
  ImBuf *ibuf;
  ImageUser iuser = *m_imageUser;

  if (m_image == nullptr) {
    return nullptr;
  }

  /* local changes to the original ImageUser */
  if (BKE_image_is_multilayer(m_image) == false) {
    iuser.multi_index = BKE_scene_multiview_view_id_get(m_rd, m_viewName);
  }

  ibuf = BKE_image_acquire_ibuf(m_image, &iuser, nullptr);
  if (ibuf == nullptr || (ibuf->rect == nullptr && ibuf->rect_float == nullptr)) {
    BKE_image_release_ibuf(m_image, ibuf, nullptr);
    return nullptr;
  }
  return ibuf;
}

void BaseImageOperation::initExecution()
{
  ImBuf *stackbuf = getImBuf();
  m_buffer = stackbuf;
  if (stackbuf) {
    m_imageFloatBuffer = stackbuf->rect_float;
    m_imageByteBuffer = stackbuf->rect;
    m_depthBuffer = stackbuf->zbuf_float;
    if (stackbuf->zbuf_float) {
      depth_buffer_ = new MemoryBuffer(stackbuf->zbuf_float, 1, stackbuf->x, stackbuf->y);
    }
    m_imagewidth = stackbuf->x;
    m_imageheight = stackbuf->y;
    m_numberOfChannels = stackbuf->channels;
  }
}

void BaseImageOperation::deinitExecution()
{
  m_imageFloatBuffer = nullptr;
  m_imageByteBuffer = nullptr;
  BKE_image_release_ibuf(m_image, m_buffer, nullptr);
  if (depth_buffer_) {
    delete depth_buffer_;
    depth_buffer_ = nullptr;
  }
}

void BaseImageOperation::determine_canvas(const rcti &UNUSED(preferred_area), rcti &r_area)
{
  ImBuf *stackbuf = getImBuf();

  r_area = COM_AREA_NONE;

  if (stackbuf) {
    BLI_rcti_init(&r_area, 0, stackbuf->x, 0, stackbuf->y);
  }

  BKE_image_release_ibuf(m_image, stackbuf, nullptr);
}

static void sampleImageAtLocation(
    ImBuf *ibuf, float x, float y, PixelSampler sampler, bool make_linear_rgb, float color[4])
{
  if (ibuf->rect_float) {
    switch (sampler) {
      case PixelSampler::Nearest:
        nearest_interpolation_color(ibuf, nullptr, color, x, y);
        break;
      case PixelSampler::Bilinear:
        bilinear_interpolation_color(ibuf, nullptr, color, x, y);
        break;
      case PixelSampler::Bicubic:
        bicubic_interpolation_color(ibuf, nullptr, color, x, y);
        break;
    }
  }
  else {
    unsigned char byte_color[4];
    switch (sampler) {
      case PixelSampler::Nearest:
        nearest_interpolation_color(ibuf, byte_color, nullptr, x, y);
        break;
      case PixelSampler::Bilinear:
        bilinear_interpolation_color(ibuf, byte_color, nullptr, x, y);
        break;
      case PixelSampler::Bicubic:
        bicubic_interpolation_color(ibuf, byte_color, nullptr, x, y);
        break;
    }
    rgba_uchar_to_float(color, byte_color);
    if (make_linear_rgb) {
      IMB_colormanagement_colorspace_to_scene_linear_v4(color, false, ibuf->rect_colorspace);
    }
  }
}

void ImageOperation::executePixelSampled(float output[4], float x, float y, PixelSampler sampler)
{
  int ix = x, iy = y;
  if (m_imageFloatBuffer == nullptr && m_imageByteBuffer == nullptr) {
    zero_v4(output);
  }
  else if (ix < 0 || iy < 0 || ix >= m_buffer->x || iy >= m_buffer->y) {
    zero_v4(output);
  }
  else {
    sampleImageAtLocation(m_buffer, x, y, sampler, true, output);
  }
}

void ImageOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                  const rcti &area,
                                                  Span<MemoryBuffer *> UNUSED(inputs))
{
  output->copy_from(m_buffer, area, true);
}

void ImageAlphaOperation::executePixelSampled(float output[4],
                                              float x,
                                              float y,
                                              PixelSampler sampler)
{
  float tempcolor[4];

  if (m_imageFloatBuffer == nullptr && m_imageByteBuffer == nullptr) {
    output[0] = 0.0f;
  }
  else {
    tempcolor[3] = 1.0f;
    sampleImageAtLocation(m_buffer, x, y, sampler, false, tempcolor);
    output[0] = tempcolor[3];
  }
}

void ImageAlphaOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                       const rcti &area,
                                                       Span<MemoryBuffer *> UNUSED(inputs))
{
  output->copy_from(m_buffer, area, 3, COM_DATA_TYPE_VALUE_CHANNELS, 0);
}

void ImageDepthOperation::executePixelSampled(float output[4],
                                              float x,
                                              float y,
                                              PixelSampler /*sampler*/)
{
  if (m_depthBuffer == nullptr) {
    output[0] = 0.0f;
  }
  else {
    if (x < 0 || y < 0 || x >= this->getWidth() || y >= this->getHeight()) {
      output[0] = 0.0f;
    }
    else {
      int offset = y * getWidth() + x;
      output[0] = m_depthBuffer[offset];
    }
  }
}

void ImageDepthOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                       const rcti &area,
                                                       Span<MemoryBuffer *> UNUSED(inputs))
{
  if (depth_buffer_) {
    output->copy_from(depth_buffer_, area);
  }
  else {
    output->fill(area, COM_VALUE_ZERO);
  }
}

}  // namespace blender::compositor
