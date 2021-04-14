/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#pragma once

#include <emulator/config/config.hpp>
#include <emulator/core/hw/dma.hpp>
#include <emulator/core/hw/interrupt.hpp>
#include <emulator/core/scheduler.hpp>
#include <cstdint>
#include <functional>

#include "registers.hpp"

namespace nba::core {

class PPU {
public:
  PPU(Scheduler& scheduler, IRQ& irq, DMA& dma, std::shared_ptr<Config> config);

  void Reset();

  // TODO: get rid of this ugly hack.
  void ResetReadingVRAM() { is_reading_vram = false; }
  bool IsReadingVRAM() { return is_reading_vram; }

  std::uint8_t pram[0x00400];
  std::uint8_t oam [0x00400];
  std::uint8_t vram[0x18000];

  struct MMIO {
    DisplayControl dispcnt;
    DisplayStatus dispstat;

    std::uint8_t vcount;

    BackgroundControl bgcnt[4] { 0, 1, 2, 3 };

    std::uint16_t bghofs[4];
    std::uint16_t bgvofs[4];

    ReferencePoint bgx[2], bgy[2];
    std::int16_t bgpa[2];
    std::int16_t bgpb[2];
    std::int16_t bgpc[2];
    std::int16_t bgpd[2];

    WindowRange winh[2];
    WindowRange winv[2];
    WindowLayerSelect winin;
    WindowLayerSelect winout;

    Mosaic mosaic;

    BlendControl bldcnt;
    int eva;
    int evb;
    int evy;
  } mmio;

private:
  friend struct DisplayStatus;

  enum ObjAttribute {
    OBJ_IS_ALPHA  = 1,
    OBJ_IS_WINDOW = 2
  };

  enum ObjectMode {
    OBJ_NORMAL = 0,
    OBJ_SEMI   = 1,
    OBJ_WINDOW = 2,
    OBJ_PROHIBITED = 3
  };

  enum Layer {
    LAYER_BG0 = 0,
    LAYER_BG1 = 1,
    LAYER_BG2 = 2,
    LAYER_BG3 = 3,
    LAYER_OBJ = 4,
    LAYER_SFX = 5,
    LAYER_BD  = 5
  };

  enum Enable {
    ENABLE_BG0 = 0,
    ENABLE_BG1 = 1,
    ENABLE_BG2 = 2,
    ENABLE_BG3 = 3,
    ENABLE_OBJ = 4,
    ENABLE_WIN0 = 5,
    ENABLE_WIN1 = 6,
    ENABLE_OBJWIN = 7
  };

  void CheckVerticalCounterIRQ();
  void OnScanlineComplete(int cycles_late);
  void OnHblankComplete(int cycles_late);
  void OnVblankScanlineComplete(int cycles_late);
  void OnVblankHblankComplete(int cycles_late);

  void BeginRenderMode0(int cycles_late);
  // TODO: rename  to BeginFetchMapDataMode0();
  void FetchMapDataMode0(int id, int cycles_late);
  void FetchMapDataMode0Next(int id, int cycles_late);
  void FetchTileDataMode0(int id, int cycles_late);

  void RenderScanline();
  void RenderLayerText(int id);
  void RenderLayerAffine(int id);
  void RenderLayerBitmap1();
  void RenderLayerBitmap2();
  void RenderLayerBitmap3();
  void RenderLayerOAM(bool bitmap_mode, int line);
  void RenderWindow(int id);

  static auto ConvertColor(std::uint16_t color) -> std::uint32_t;

  template<bool window, bool blending>
  void ComposeScanlineTmpl(int bg_min, int bg_max);
  void ComposeScanline(int bg_min, int bg_max);
  void Blend(std::uint16_t& target1, std::uint16_t target2, BlendControl::Effect sfx);

  #include "helper.inl"

  Scheduler& scheduler;
  IRQ& irq;
  DMA& dma;
  std::shared_ptr<Config> config;

  // TODO: properly reset this structure on Reset().
  struct Renderer {
    struct Background {
      // Mode 0 map fetcher
      std::uint32_t base;
      std::uint32_t base_adjust;
      int grid_x;
      int grid_y;

      struct Tile {
        int  number;
        int  palette;
        bool flip_x;
        bool flip_y;
      } tile;

      int tile_x;

      // This is just a test for now.
      int draw_x;
    } bg[4];
  } renderer = {};

  // TODO: properly reset this on Reset().
  bool is_reading_vram = false;

  std::uint16_t buffer_bg[4][240];

  bool line_contains_alpha_obj;

  struct ObjectPixel {
    std::uint16_t color;
    std::uint8_t  priority;
    unsigned alpha  : 1;
    unsigned window : 1;
  } buffer_obj[240];

  bool buffer_win[2][240];
  bool window_scanline_enable[2];

  std::uint32_t output[240*160];

  static constexpr std::uint16_t s_color_transparent = 0x8000;
  static const int s_obj_size[4][4][2];
};

} // namespace nba::core
