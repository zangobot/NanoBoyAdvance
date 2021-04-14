/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include <cstring>

#include "ppu.hpp"

namespace nba::core {

PPU::PPU(Scheduler& scheduler, IRQ& irq, DMA& dma, std::shared_ptr<Config> config)
    : scheduler(scheduler)
    , irq(irq)
    , dma(dma)
    , config(config) {
  Reset();
  mmio.dispstat.ppu = this;
}

void PPU::Reset() {
  std::memset(pram, 0, 0x00400);
  std::memset(oam,  0, 0x00400);
  std::memset(vram, 0, 0x18000);

  mmio.dispcnt.Reset();
  mmio.dispstat.Reset();
  mmio.vcount = 0;

  for (int i = 0; i < 4; i++) {
    mmio.bgcnt[i].Reset();
    mmio.bghofs[i] = 0;
    mmio.bgvofs[i] = 0;

    if (i < 2) {
      mmio.bgx[i].Reset();
      mmio.bgy[i].Reset();
      mmio.bgpa[i] = 0x100;
      mmio.bgpb[i] = 0;
      mmio.bgpc[i] = 0;
      mmio.bgpd[i] = 0x100;
    }
  }

  mmio.winh[0].Reset();
  mmio.winh[1].Reset();
  mmio.winv[0].Reset();
  mmio.winv[1].Reset();
  mmio.winin.Reset();
  mmio.winout.Reset();

  mmio.mosaic.Reset();

  mmio.eva = 0;
  mmio.evb = 0;
  mmio.evy = 0;
  mmio.bldcnt.Reset();

  // Todo: clean this gross hack up.
  mmio.vcount = 0xFF;
  OnHblankComplete(0);
  // scheduler.Add(1006, this, &PPU::OnScanlineComplete);
}

void PPU::CheckVerticalCounterIRQ() {
  auto& dispstat = mmio.dispstat;
  auto vcount_flag_new = dispstat.vcount_setting == mmio.vcount;

  if (dispstat.vcount_irq_enable && !dispstat.vcount_flag && vcount_flag_new) {
    irq.Raise(IRQ::Source::VCount);
  }
  
  dispstat.vcount_flag = vcount_flag_new;
}

void PPU::OnScanlineComplete(int cycles_late) {
  auto& bgx = mmio.bgx;
  auto& bgy = mmio.bgy;
  auto& bgpb = mmio.bgpb;
  auto& bgpd = mmio.bgpd;
  auto& mosaic = mmio.mosaic;

  // Note: this is just a temporary workaround. Eventually get fully rid of this?
  RenderScanline();

  scheduler.Add(226 - cycles_late, this, &PPU::OnHblankComplete);

  mmio.dispstat.hblank_flag = 1;

  if (mmio.dispstat.hblank_irq_enable) {
    irq.Raise(IRQ::Source::HBlank);
  }

  dma.Request(DMA::Occasion::HBlank);
  
  if (mmio.vcount >= 2) {
    dma.Request(DMA::Occasion::Video);
  }

  // Advance vertical background mosaic counter
  if (++mosaic.bg._counter_y == mosaic.bg.size_y) {
    mosaic.bg._counter_y = 0;
  }

  // Advance vertical OBJ mosaic counter
  if (++mosaic.obj._counter_y == mosaic.obj.size_y) {
    mosaic.obj._counter_y = 0;
  }

  /* Mode 0 doesn't have any affine backgrounds,
   * in that case the internal registers seemingly aren't updated.
   * TODO: needs more research, e.g. what happens if all affine backgrounds are disabled?
   */
  if (mmio.dispcnt.mode != 0) {
    // Advance internal affine registers and apply vertical mosaic if applicable.
    for (int i = 0; i < 2; i++) {
      if (mmio.bgcnt[2 + i].mosaic_enable) {
        if (mosaic.bg._counter_y == 0) {
          bgx[i]._current += mosaic.bg.size_y * bgpb[i];
          bgy[i]._current += mosaic.bg.size_y * bgpd[i];
        }
      } else {
        bgx[i]._current += bgpb[i];
        bgy[i]._current += bgpd[i];
      }
    }
  }
}

void PPU::OnHblankComplete(int cycles_late) {
  auto& dispcnt = mmio.dispcnt;
  auto& dispstat = mmio.dispstat;
  auto& vcount = mmio.vcount;
  auto& bgx = mmio.bgx;
  auto& bgy = mmio.bgy;
  auto& mosaic = mmio.mosaic;

  dispstat.hblank_flag = 0;
  vcount++;
  CheckVerticalCounterIRQ();

  if (dispcnt.enable[ENABLE_WIN0]) {
    RenderWindow(0);
  }

  if (dispcnt.enable[ENABLE_WIN1]) {
    RenderWindow(1);
  }

  if (vcount == 160) {
    config->video_dev->Draw(output);

    scheduler.Add(1006 - cycles_late, this, &PPU::OnVblankScanlineComplete);
    dma.Request(DMA::Occasion::VBlank);
    dispstat.vblank_flag = 1;

    if (dispstat.vblank_irq_enable) {
      irq.Raise(IRQ::Source::VBlank);
    }

    // Reset vertical mosaic counters
    mosaic.bg._counter_y = 0;
    mosaic.obj._counter_y = 0;

    // Reload internal affine registers
    bgx[0]._current = bgx[0].initial;
    bgy[0]._current = bgy[0].initial;
    bgx[1]._current = bgx[1].initial;
    bgy[1]._current = bgy[1].initial;
  } else {
    if (mmio.dispcnt.mode == 0) {
      // Note: what exactly is the delay? this could also be 14 cycles (46 - 32)?!?
      scheduler.Add(32 - cycles_late, this, &PPU::BeginRenderMode0);
    }
    // Moved to OnScanlineComplete() so that it uses per-pixel rendered data...
    // RenderScanline();

    scheduler.Add(1006 - cycles_late, this, &PPU::OnScanlineComplete);
    // Render OBJs for the *next* scanline.
    if (mmio.dispcnt.enable[ENABLE_OBJ]) {
      RenderLayerOAM(mmio.dispcnt.mode >= 3, mmio.vcount + 1);
    }
  }
}

void PPU::OnVblankScanlineComplete(int cycles_late) {
  auto& dispstat = mmio.dispstat;

  scheduler.Add(226 - cycles_late, this, &PPU::OnVblankHblankComplete);

  dispstat.hblank_flag = 1;

  if (mmio.vcount < 162) {
    dma.Request(DMA::Occasion::Video);
  } else if (mmio.vcount == 162) {
    dma.StopVideoXferDMA();
  }

  if (dispstat.hblank_irq_enable) {
    irq.Raise(IRQ::Source::HBlank);
  }
}

void PPU::OnVblankHblankComplete(int cycles_late) {
  auto& vcount = mmio.vcount;
  auto& dispstat = mmio.dispstat;

  dispstat.hblank_flag = 0;

  if (vcount == 227) {
    scheduler.Add(1006 - cycles_late, this, &PPU::OnScanlineComplete);
    vcount = 0;
  } else {
    scheduler.Add(1006 - cycles_late, this, &PPU::OnVblankScanlineComplete);
    if (++vcount == 227) {
      dispstat.vblank_flag = 0;
      // Render OBJs for the *next* scanline
      if (mmio.dispcnt.enable[ENABLE_OBJ]) {
        RenderLayerOAM(mmio.dispcnt.mode >= 3, 0);
      }
    }
  }

  if (mmio.dispcnt.enable[ENABLE_WIN0]) {
    RenderWindow(0);
  }

  if (mmio.dispcnt.enable[ENABLE_WIN1]) {
    RenderWindow(1);
  }

  if (vcount == 0) {
    // Moved to OnScanlineComplete() so that it uses per-pixel rendered data...
    // RenderScanline();

    if (mmio.dispcnt.mode == 0) {
      // Note: what exactly is the delay? this could also be 14 cycles (46 - 32)?!?
      scheduler.Add(32 - cycles_late, this, &PPU::BeginRenderMode0);
    }
  }

  CheckVerticalCounterIRQ();
}

void PPU::BeginRenderMode0(int cycles_late) {
  // TODO: cut the cycles_late bullshit, it doesn't matter anymore.
  if (mmio.dispcnt.enable[0]) {
    FetchMapDataMode0(0, cycles_late);
  }

  if (mmio.dispcnt.enable[1]) {
    // TODO: do not generate a lambda every time we schedule this event.
    scheduler.Add(1 - cycles_late, [this](int cycles_late) {
      FetchMapDataMode0(1, cycles_late);
    });
  }

  if (mmio.dispcnt.enable[2]) {
    // TODO: do not generate a lambda every time we schedule this event.
    scheduler.Add(2 - cycles_late, [this](int cycles_late) {
      FetchMapDataMode0(2, cycles_late);
    });
  }

  if (mmio.dispcnt.enable[3]) {
    // TODO: do not generate a lambda every time we schedule this event.
    scheduler.Add(3 - cycles_late, [this](int cycles_late) {
      FetchMapDataMode0(3, cycles_late);
    });
  }

}

void PPU::FetchMapDataMode0(int id, int cycles_late) {
  auto const& bgcnt  = mmio.bgcnt[id];
  auto const& mosaic = mmio.mosaic.bg;

  auto& bg = renderer.bg[id];

  int line = mmio.bgvofs[id] + mmio.vcount;
  if (bgcnt.mosaic_enable) {
    line -= mosaic._counter_y;
  }

  bg.grid_x = mmio.bghofs[id] >> 3;
  bg.grid_y = line >> 3;
  
  int screen_x = (bg.grid_x >> 5) & 1;
  int screen_y = (bg.grid_y >> 5) & 1;

  bg.grid_x &= 31;
  bg.base = (bgcnt.map_block * 2048) + ((bg.grid_y & 31) * 64);

  switch (bgcnt.size) {
    case 0:
      bg.base_adjust = 0;
      break;
    case 1: 
      bg.base += screen_x * 2048;
      bg.base_adjust = 2048;
      break;
    case 2:
      bg.base += screen_y * 2048;
      bg.base_adjust = 0;
      break;
    case 3:
      bg.base += (screen_x * 2048) + (screen_y * 4096);
      bg.base_adjust = 2048;
      break;
  }
  
  if (screen_x == 1) {
    bg.base_adjust *= -1;
  }
  
  // This is just a test for now; possibily remove later.
  bg.draw_x = 0;
  bg.tile_x = mmio.bghofs[id] & 7;

  FetchMapDataMode0Next(id, cycles_late);
}

void PPU::FetchMapDataMode0Next(int id, int cycles_late) {
  auto const& bgcnt  = mmio.bgcnt[id];
  auto const& mosaic = mmio.mosaic.bg;

  auto& bg = renderer.bg[id];

  std::uint32_t offset  = bg.base + bg.grid_x * 2;
  std::uint16_t encoder = (vram[offset + 1] << 8) | vram[offset];

  bg.tile.number  = encoder & 0x3FF;
  bg.tile.palette = encoder >> 12;
  bg.tile.flip_x  = encoder & (1 << 10);
  bg.tile.flip_y  = encoder & (1 << 11);

  if (++bg.grid_x == 32) {
    bg.grid_x = 0;
    bg.base += bg.base_adjust;
    bg.base_adjust *= -1;
  }

  // TODO: do not generate a lambda every time we schedule this event.
  scheduler.Add(4 - cycles_late, [this, id](int cycles_late) {
    FetchTileDataMode0(id, cycles_late);
  });

  is_reading_vram = true;
}

void PPU::FetchTileDataMode0(int id, int cycles_late) {
  auto const& bgcnt  = mmio.bgcnt[id];
  auto const& mosaic = mmio.mosaic.bg;

  auto& bg = renderer.bg[id];

  // TODO: is this latched?
  std::uint32_t tile_base = bgcnt.tile_block * 16384;

  // TODO: this is duplicate. also is this value latched?
  int line = mmio.bgvofs[id] + mmio.vcount;
  if (bgcnt.mosaic_enable) {
    line -= mosaic._counter_y;
  }

  int tile_x = bg.tile_x;
  int tile_y = line & 7;

  if (bg.tile.flip_x) tile_x ^= 7;
  if (bg.tile.flip_y) tile_y ^= 7;

  if (bgcnt.full_palette) {
    auto data = vram[tile_base + bg.tile.number * 64 + tile_y * 8 + tile_x];

    if (data == 0) {
      buffer_bg[id][bg.draw_x] = s_color_transparent;      
    } else {
      buffer_bg[id][bg.draw_x] = ReadPalette(0, data);
    }
  } else {
    auto data = vram[tile_base + bg.tile.number * 32 + tile_y * 4 + (tile_x >> 1)];
    
    if (tile_x & 1) {
      data >>= 4;
    } else {
      data &= 15;
    }

    if (data == 0) {
      buffer_bg[id][bg.draw_x] = s_color_transparent;
    } else {
      buffer_bg[id][bg.draw_x] = ReadPalette(bg.tile.palette, data);
    }
  }

  if (++bg.draw_x != 240) {
    if (++bg.tile_x == 8) {
      bg.tile_x = 0;

      // TODO: do not generate a lambda every time we schedule this event.
      scheduler.Add(4 - cycles_late, [this, id](int cycles_late) {
        FetchMapDataMode0Next(id, cycles_late);
      });
    } else {
      // TODO: do not generate a lambda every time we schedule this event.
      scheduler.Add(4 - cycles_late, [this, id](int cycles_late) {
        FetchTileDataMode0(id, cycles_late);
      });
    }
  }

  is_reading_vram = true;
}

} // namespace nba::core
