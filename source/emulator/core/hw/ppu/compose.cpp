/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include <algorithm>

#include "ppu.hpp"

namespace nba::core {

using BlendMode = BlendControl::Effect;

auto PPU::ConvertColor(std::uint16_t color) -> std::uint32_t {
  int r = (color >>  0) & 0x1F;
  int g = (color >>  5) & 0x1F;
  int b = (color >> 10) & 0x1F;

  return r << 19 |
         g << 11 |
         b <<  3 |
         0xFF000000;
}

void PPU::InitBlendTable() {
  for (int color0 = 0; color0 <= 31; color0++) {
    for (int color1 = 0; color1 <= 31; color1++) {
      for (int factor0 = 0; factor0 <= 16; factor0++) {
        for (int factor1 = 0; factor1 <= 16; factor1++) {
          int result = (color0 * factor0 + color1 * factor1) >> 4;

          blend_table[factor0][factor1][color0][color1] = std::min<std::uint8_t>(result, 31);
        }
      }
    }
  }
}

void PPU::RenderScanline(int line) {
  std::uint32_t* buffer = &output[line * 240];

  if (mmio_copy[line].dispcnt.forced_blank) {
    for (int x = 0; x < 240; x++) {
      buffer[x] = ConvertColor(0x7FFF);
    }
    return;
  }

  if (mmio_copy[line].dispcnt.enable[ENABLE_WIN0]) {
    RenderWindow(line, 0);
  }

  if (mmio_copy[line].dispcnt.enable[ENABLE_WIN1]) {
    RenderWindow(line, 1);
  }

  switch (mmio_copy[line].dispcnt.mode) {
    case 0: {
      /* BG Mode 0 - 240x160 pixels, Text mode */
      for (int i = 0; i < 4; i++) {
        if (mmio_copy[line].dispcnt.enable[i]) {
          RenderLayerText(line, i);
        }
      }
      if (mmio_copy[line].dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(line, false);
      }
      ComposeScanline(line, 0, 3);
      break;
    }
    case 1: {
      /* BG Mode 1 - 240x160 pixels, Text and RS mode mixed */
      for (int i = 0; i < 2; i++) {
        if (mmio_copy[line].dispcnt.enable[i]) {
          RenderLayerText(line, i);
        }
      }
      if (mmio_copy[line].dispcnt.enable[ENABLE_BG2]) {
        RenderLayerAffine(line, 0);
      }
      if (mmio_copy[line].dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(line, false);
      }
      ComposeScanline(line, 0, 2);
      break;
    }
    case 2: {
      /* BG Mode 2 - 240x160 pixels, RS mode */
      for (int i = 0; i < 2; i++) {
        if (mmio_copy[line].dispcnt.enable[2 + i]) {
          RenderLayerAffine(line, i);
        }
      }
      if (mmio_copy[line].dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(line, false);
      }
      ComposeScanline(line, 2, 3);
      break;
    }
    case 3: {
      /* BG Mode 3 - 240x160 pixels, 32768 colors */
      if (mmio_copy[line].dispcnt.enable[2]) {
        RenderLayerBitmap1(line);
      }
      if (mmio_copy[line].dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(line, true);
      }
      ComposeScanline(line, 2, 2);
      break;
    }
    case 4: {
      /* BG Mode 4 - 240x160 pixels, 256 colors (out of 32768 colors) */
      if (mmio_copy[line].dispcnt.enable[2]) {
        RenderLayerBitmap2(line);
      }
      if (mmio_copy[line].dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(line, true);
      }
      ComposeScanline(line, 2, 2);
      break;
    }
    case 5: {
      /* BG Mode 5 - 160x128 pixels, 32768 colors */
      if (mmio_copy[line].dispcnt.enable[2]) {
        RenderLayerBitmap3(line);
      }
      if (mmio_copy[line].dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(line, true);
      }
      ComposeScanline(line, 2, 2);
      break;
    }
  }
}

void PPU::ComposeScanline(int line, int bg_min, int bg_max) {
  std::uint32_t* buffer = &output[line * 240];
  std::uint16_t backdrop = ReadPalette(0, 0);

  auto const& dispcnt = mmio_copy[line].dispcnt;
  auto const& bgcnt = mmio_copy[line].bgcnt;
  auto const& winin = mmio_copy[line].winin;
  auto const& winout = mmio_copy[line].winout;

  bool win0_active = dispcnt.enable[ENABLE_WIN0] && window_scanline_enable[0];
  bool win1_active = dispcnt.enable[ENABLE_WIN1] && window_scanline_enable[1];
  bool win2_active = dispcnt.enable[ENABLE_OBJWIN];
  bool no_windows  = !dispcnt.enable[ENABLE_WIN0] &&
                     !dispcnt.enable[ENABLE_WIN1] &&
                     !dispcnt.enable[ENABLE_OBJWIN];

  int bg_list[4];
  int bg_count = 0;

  /* Sort enabled backgrounds by their respective priority in ascending order. */
  for (int prio = 3; prio >= 0; prio--) {
    for (int bg = bg_max; bg >= bg_min; bg--) {
      if (dispcnt.enable[bg] && bgcnt[bg].priority == prio) {
        bg_list[bg_count++] = bg;
      }
    }
  }

  std::uint16_t pixel[2];

  for (int x = 0; x < 240; x++) {
    int prio[2] = { 4, 4 };
    int layer[2] = { LAYER_BD, LAYER_BD };

    const int* win_layer_enable = winout.enable[0];

    /* Determine the window that has the highest priority. */
    if (win0_active && buffer_win[0][x]) {
      win_layer_enable = winin.enable[0];
    } else if (win1_active && buffer_win[1][x]) {
      win_layer_enable = winin.enable[1];
    } else if (win2_active && buffer_obj[x].window) {
      win_layer_enable = winout.enable[1];
    }

    /* Find up to two top-most visible background pixels. */
    for (int i = 0; i < bg_count; i++) {
      int bg = bg_list[i];

      if (no_windows || win_layer_enable[bg]) {
        auto pixel_new = buffer_bg[bg][x];
        if (pixel_new != s_color_transparent) {
          layer[1] = layer[0];
          layer[0] = bg;
          prio[1] = prio[0];
          prio[0] = bgcnt[bg].priority;
        }
      }
    }

    /* Check if a OBJ pixel takes priority over one of the two
     * top-most background pixels and insert it accordingly.
     */
    if (dispcnt.enable[ENABLE_OBJ] &&
        buffer_obj[x].color != s_color_transparent &&
        (no_windows || win_layer_enable[LAYER_OBJ])) {
      int priority = buffer_obj[x].priority;

      if (priority <= prio[0]) {
        layer[1] = layer[0];
        layer[0] = LAYER_OBJ;
      } else if (priority <= prio[1]) {
        layer[1] = LAYER_OBJ;
      }
    }

    /* Map layer numbers to pixels. */
    for (int i = 0; i < 2; i++) {
      int _layer = layer[i];
      switch (_layer) {
        case 0:
        case 1:
        case 2:
        case 3:
          pixel[i] = buffer_bg[_layer][x];
          break;
        case 4:
          pixel[i] = buffer_obj[x].color;
          break;
        case 5:
          pixel[i] = backdrop;
          break;
      }
    }

    bool is_alpha_obj = layer[0] == LAYER_OBJ && buffer_obj[x].alpha;

    if (no_windows || win_layer_enable[LAYER_SFX] || is_alpha_obj) {
      auto blend_mode = mmio_copy[line].bldcnt.sfx;
      bool have_dst = mmio_copy[line].bldcnt.targets[0][layer[0]];
      bool have_src = mmio_copy[line].bldcnt.targets[1][layer[1]];

      if (is_alpha_obj && have_src) {
        Blend(line, pixel[0], pixel[1], BlendMode::SFX_BLEND);
      } else if (have_dst && blend_mode != BlendMode::SFX_NONE && (have_src || blend_mode != BlendMode::SFX_BLEND)) {
        Blend(line, pixel[0], pixel[1], blend_mode);
      }
    }

    buffer[x] = ConvertColor(pixel[0]);
  }
}

void PPU::Blend(int line,
                std::uint16_t& target1,
                std::uint16_t target2,
                BlendMode sfx) {
  int r1 = (target1 >>  0) & 0x1F;
  int g1 = (target1 >>  5) & 0x1F;
  int b1 = (target1 >> 10) & 0x1F;

  switch (sfx) {
    case BlendMode::SFX_BLEND: {
      int eva = std::min<int>(16, mmio_copy[line].eva);
      int evb = std::min<int>(16, mmio_copy[line].evb);

      int r2 = (target2 >>  0) & 0x1F;
      int g2 = (target2 >>  5) & 0x1F;
      int b2 = (target2 >> 10) & 0x1F;

      r1 = blend_table[eva][evb][r1][r2];
      g1 = blend_table[eva][evb][g1][g2];
      b1 = blend_table[eva][evb][b1][b2];
      break;
    }
    case BlendMode::SFX_BRIGHTEN: {
      int evy = std::min<int>(16, mmio_copy[line].evy);

      r1 = blend_table[16 - evy][evy][r1][31];
      g1 = blend_table[16 - evy][evy][g1][31];
      b1 = blend_table[16 - evy][evy][b1][31];
      break;
    }
    case BlendMode::SFX_DARKEN: {
      int evy = std::min<int>(16, mmio_copy[line].evy);

      r1 = blend_table[16 - evy][evy][r1][0];
      g1 = blend_table[16 - evy][evy][g1][0];
      b1 = blend_table[16 - evy][evy][b1][0];
      break;
    }
  }

  target1 = r1 | (g1 << 5) | (b1 << 10);
}

} // namespace nba::core
