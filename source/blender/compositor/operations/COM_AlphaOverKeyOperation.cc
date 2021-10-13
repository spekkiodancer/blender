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

#include "COM_AlphaOverKeyOperation.h"

namespace blender::compositor {

AlphaOverKeyOperation::AlphaOverKeyOperation()
{
  this->flags.can_be_constant = true;
}

void AlphaOverKeyOperation::executePixelSampled(float output[4],
                                                float x,
                                                float y,
                                                PixelSampler sampler)
{
  float inputColor1[4];
  float inputOverColor[4];
  float value[4];

  m_inputValueOperation->readSampled(value, x, y, sampler);
  m_inputColor1Operation->readSampled(inputColor1, x, y, sampler);
  m_inputColor2Operation->readSampled(inputOverColor, x, y, sampler);

  if (inputOverColor[3] <= 0.0f) {
    copy_v4_v4(output, inputColor1);
  }
  else if (value[0] == 1.0f && inputOverColor[3] >= 1.0f) {
    copy_v4_v4(output, inputOverColor);
  }
  else {
    float premul = value[0] * inputOverColor[3];
    float mul = 1.0f - premul;

    output[0] = (mul * inputColor1[0]) + premul * inputOverColor[0];
    output[1] = (mul * inputColor1[1]) + premul * inputOverColor[1];
    output[2] = (mul * inputColor1[2]) + premul * inputOverColor[2];
    output[3] = (mul * inputColor1[3]) + value[0] * inputOverColor[3];
  }
}

void AlphaOverKeyOperation::update_memory_buffer_row(PixelCursor &p)
{
  for (; p.out < p.row_end; p.next()) {
    const float *color1 = p.color1;
    const float *over_color = p.color2;
    const float value = *p.value;

    if (over_color[3] <= 0.0f) {
      copy_v4_v4(p.out, color1);
    }
    else if (value == 1.0f && over_color[3] >= 1.0f) {
      copy_v4_v4(p.out, over_color);
    }
    else {
      const float premul = value * over_color[3];
      const float mul = 1.0f - premul;

      p.out[0] = (mul * color1[0]) + premul * over_color[0];
      p.out[1] = (mul * color1[1]) + premul * over_color[1];
      p.out[2] = (mul * color1[2]) + premul * over_color[2];
      p.out[3] = (mul * color1[3]) + value * over_color[3];
    }
  }
}

}  // namespace blender::compositor
