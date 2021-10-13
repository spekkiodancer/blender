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

#pragma once

#include "BKE_global.h"
#include "BLI_rect.h"
#include "COM_MultiThreadedOperation.h"
#include "DNA_image_types.h"

namespace blender::compositor {

class ViewerOperation : public MultiThreadedOperation {
 private:
  /* TODO(manzanilla): To be removed together with tiled implementation. */
  float *m_outputBuffer;
  float *m_depthBuffer;

  Image *m_image;
  ImageUser *m_imageUser;
  bool m_active;
  float m_centerX;
  float m_centerY;
  ChunkOrdering m_chunkOrder;
  bool m_doDepthBuffer;
  ImBuf *m_ibuf;
  bool m_useAlphaInput;
  const RenderData *m_rd;
  const char *m_viewName;

  const ColorManagedViewSettings *m_viewSettings;
  const ColorManagedDisplaySettings *m_displaySettings;

  SocketReader *m_imageInput;
  SocketReader *m_alphaInput;
  SocketReader *m_depthInput;

  int display_width_;
  int display_height_;

 public:
  ViewerOperation();
  void initExecution() override;
  void deinitExecution() override;
  void executeRegion(rcti *rect, unsigned int tileNumber) override;
  void determine_canvas(const rcti &preferred_area, rcti &r_area) override;
  bool isOutputOperation(bool /*rendering*/) const override
  {
    if (G.background) {
      return false;
    }
    return isActiveViewerOutput();
  }
  void setImage(Image *image)
  {
    m_image = image;
  }
  void setImageUser(ImageUser *imageUser)
  {
    m_imageUser = imageUser;
  }
  bool isActiveViewerOutput() const override
  {
    return m_active;
  }
  void setActive(bool active)
  {
    m_active = active;
  }
  void setCenterX(float centerX)
  {
    m_centerX = centerX;
  }
  void setCenterY(float centerY)
  {
    m_centerY = centerY;
  }
  void setChunkOrder(ChunkOrdering tileOrder)
  {
    m_chunkOrder = tileOrder;
  }
  float getCenterX() const
  {
    return m_centerX;
  }
  float getCenterY() const
  {
    return m_centerY;
  }
  ChunkOrdering getChunkOrder() const
  {
    return m_chunkOrder;
  }
  eCompositorPriority getRenderPriority() const override;
  void setUseAlphaInput(bool value)
  {
    m_useAlphaInput = value;
  }
  void setRenderData(const RenderData *rd)
  {
    m_rd = rd;
  }
  void setViewName(const char *viewName)
  {
    m_viewName = viewName;
  }

  void setViewSettings(const ColorManagedViewSettings *viewSettings)
  {
    m_viewSettings = viewSettings;
  }
  void setDisplaySettings(const ColorManagedDisplaySettings *displaySettings)
  {
    m_displaySettings = displaySettings;
  }

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;

  void clear_display_buffer();

 private:
  void updateImage(const rcti *rect);
  void initImage();
};

}  // namespace blender::compositor
