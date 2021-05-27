/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include <common/likely.hpp>
#include <emulator/core/cpu-mmio.hpp>

#include "dma.hpp"

namespace nba::core {

static constexpr int g_dma_src_modify[2][4] = {
  { 2, -2, 0, 0 },
  { 4, -4, 0, 0 }
};

static constexpr int g_dma_dst_modify[2][4] = {
  { 2, -2, 0, 2 },
  { 4, -4, 0, 4 }
};

static constexpr int g_dma_none_id = -1;

// NOTE: Retrieves DMA with highest priority from a DMA bitset.
static constexpr int g_dma_from_bitset[] = {
  /* 0b0000 */ g_dma_none_id,
  /* 0b0001 */ 0,
  /* 0b0010 */ 1,
  /* 0b0011 */ 0,
  /* 0b0100 */ 2,
  /* 0b0101 */ 0,
  /* 0b0110 */ 1,
  /* 0b0111 */ 0,
  /* 0b1000 */ 3,
  /* 0b1001 */ 0,
  /* 0b1010 */ 1,
  /* 0b1011 */ 0,
  /* 0b1100 */ 2,
  /* 0b1101 */ 0,
  /* 0b1110 */ 1,
  /* 0b1111 */ 0
};

static constexpr std::uint32_t g_dma_src_mask[] = {
  0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF
};

static constexpr std::uint32_t g_dma_dst_mask[] = {
  0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF
};

static constexpr std::uint32_t g_dma_len_mask[] = {
  0x3FFF, 0x3FFF, 0x3FFF, 0xFFFF
};

void DMA::Reset() {
  active_dma_id = g_dma_none_id;
  early_exit_trigger = false;
  hblank_set = 0;
  vblank_set = 0;
  video_set = 0;
  runnable_set = 0;

  for (int id = 0; id < 4; id++) {
    channels[id] = {};
    channels[id].id = id;
  }
}

void DMA::ScheduleDMAs(unsigned int bitset) {
  while (bitset > 0) {
    auto chan_id = g_dma_from_bitset[bitset];
    bitset &= ~(1 << chan_id);
    channels[chan_id].startup_event = scheduler.Add(2, [this, chan_id](int cycles_late) {
      channels[chan_id].startup_event = nullptr;
      if (runnable_set.none()) {
        active_dma_id = chan_id;
      } else if (chan_id < active_dma_id) {
        active_dma_id = chan_id;
        early_exit_trigger = true;
      }
      runnable_set.set(chan_id, true);
    });
  }
}

void DMA::SelectNextDMA() {
  active_dma_id = g_dma_from_bitset[runnable_set.to_ulong()];
}

void DMA::Request(Occasion occasion) {
  switch (occasion) {
    case Occasion::HBlank:
      ScheduleDMAs(hblank_set.to_ulong());
      break;
    case Occasion::VBlank:
      ScheduleDMAs(vblank_set.to_ulong());
      break;
    case Occasion::Video:
      ScheduleDMAs(video_set.to_ulong());
      break;
    case Occasion::FIFO0:
    case Occasion::FIFO1: {
      auto address = (occasion == Occasion::FIFO0) ? FIFO_A : FIFO_B;
      for (int chan_id = 1; chan_id <= 2; chan_id++) {
        auto& channel = channels[chan_id];
        if (channel.enable &&
            channel.time == Channel::Special &&
            channel.dst_addr == address) {
          ScheduleDMAs(1 << chan_id);
        }
      }
      break;
    }
  }
}

void DMA::StopVideoXferDMA() {
  auto& channel = channels[3];

  if (channel.enable && channel.time == Channel::Timing::Special) {
    // TODO: cancel startup event? Is this actually correct at all?
    channel.enable = false;
    runnable_set.set(3, false);
    video_set.set(3, false);
    SelectNextDMA();
  }
}

void DMA::Run() {
  if (!IsRunning())
    return;
  RunChannel(true);
  while (IsRunning()) {
    RunChannel(false);
  }
  //RunChannel(true);
}

void DMA::RunChannel(bool first) {
  auto& channel = channels[active_dma_id];
  int dst_modify;
  int src_modify;
  auto size = channel.size;
  auto access = Access::Nonsequential;

  // TODO: we are caching the size, source and destination address delta.
  // But is it possible for a DMA channel to change its own configuration?
  if (channel.is_fifo_dma) {
    size = Channel::Size::Word;
    dst_modify = 0;
    src_modify = g_dma_src_modify[size][channel.src_cntl];
  } else {
    dst_modify = g_dma_dst_modify[size][channel.dst_cntl];
    src_modify = g_dma_src_modify[size][channel.src_cntl];
  }

  // TODO: I don't know how the internal processing time for DMA is supposed to work.
  // This is what works best so far, but I don't think that it's correct. Needs revision.
  auto src_page = GetUnaliasedMemoryArea(channel.src_addr >> 24);
  auto dst_page = GetUnaliasedMemoryArea(channel.dst_addr >> 24);
  if ((src_page != 0x08 || dst_page != 0x08) && first) {
    memory.Idle();
    memory.Idle();
  }

  while (channel.latch.length != 0) {
    if (early_exit_trigger) {
      early_exit_trigger = false;
      return;
    }

    if (size == Channel::Half) {
      std::uint16_t value;

      if (likely(channel.latch.src_addr >= 0x02000000)) {
        value = memory.ReadHalf(channel.latch.src_addr, access);
        channel.latch.bus = (value << 16) | value;
        latch = channel.latch.bus;
      } else {
        if (channel.latch.dst_addr & 2) {
          value = channel.latch.bus >> 16;
        } else {
          value = channel.latch.bus;
        }
        memory.Idle();
      }

      memory.WriteHalf(channel.latch.dst_addr, value, access);
    } else {
      if (likely(channel.latch.src_addr >= 0x02000000)) {
        channel.latch.bus = memory.ReadWord(channel.latch.src_addr, access);
        latch = channel.latch.bus;
      } else {
        memory.Idle();
      }
      memory.WriteWord(channel.latch.dst_addr, channel.latch.bus, access);
    }

    channel.latch.src_addr += src_modify;
    channel.latch.dst_addr += dst_modify;
    channel.latch.length--;

    access = Access::Sequential;
  }

  runnable_set.set(channel.id, false);

  if (channel.interrupt) {
    irq.Raise(IRQ::Source::DMA, channel.id);
  }

  if (channel.repeat) {
    if (channel.is_fifo_dma) {
      channel.latch.length = 4;
    } else {
      channel.latch.length = channel.length & g_dma_len_mask[channel.id];
      if (channel.latch.length == 0) {
        channel.latch.length = g_dma_len_mask[channel.id] + 1;
      }
    }

    if (channel.dst_cntl == Channel::Reload && !channel.is_fifo_dma) {
      auto mask = channel.size == Channel::Word ? ~3 : ~1;
      channel.latch.dst_addr = channel.dst_addr & mask;
    }
  } else {
    channel.enable = false;
    hblank_set.set(channel.id, false);
    vblank_set.set(channel.id, false);
    video_set.set(channel.id, false);
  }

  SelectNextDMA();
}

auto DMA::Read(int chan_id, int offset) -> std::uint8_t {
  auto const& channel = channels[chan_id];

  switch (offset) {
    case REG_DMAXCNT_H | 0: {
      return (channel.dst_cntl << 5) |
             (channel.src_cntl << 7);
    }
    case REG_DMAXCNT_H | 1: {
      return (channel.src_cntl  >> 1) |
             (channel.size      << 2) |
             (channel.time      << 4) |
             (channel.repeat    ? 2   : 0) |
             (channel.gamepak   ? 8   : 0) |
             (channel.interrupt ? 64  : 0) |
             (channel.enable    ? 128 : 0);
    }
    default: return 0;
  }
}

void DMA::Write(int chan_id, int offset, std::uint8_t value) {
  auto& channel = channels[chan_id];

  switch (offset) {
    case REG_DMAXSAD | 0:
    case REG_DMAXSAD | 1:
    case REG_DMAXSAD | 2:
    case REG_DMAXSAD | 3: {
      int shift = offset * 8;
      channel.src_addr &= ~(0xFFUL << shift);
      channel.src_addr |= (value << shift) & g_dma_src_mask[chan_id];
      break;
    }
    case REG_DMAXDAD | 0:
    case REG_DMAXDAD | 1:
    case REG_DMAXDAD | 2:
    case REG_DMAXDAD | 3: {
      int shift = (offset - 4) * 8;
      channel.dst_addr &= ~(0xFFUL << shift);
      channel.dst_addr |= (value << shift) & g_dma_dst_mask[chan_id];
      break;
    }
    case REG_DMAXCNT_L | 0: channel.length = (channel.length & 0xFF00) | (value << 0); break;
    case REG_DMAXCNT_L | 1: channel.length = (channel.length & 0x00FF) | (value << 8); break;
    case REG_DMAXCNT_H | 0: {
      channel.dst_cntl = static_cast<Channel::Control>((value >> 5) & 3);
      channel.src_cntl = static_cast<Channel::Control>((channel.src_cntl & 0b10) | (value>>7));
      break;
    }
    case REG_DMAXCNT_H | 1: {
      bool enable_old = channel.enable;

      // TODO: check that the actual repeat bit is masked if immediate transfer is selected.
      channel.src_cntl  = Channel::Control((channel.src_cntl & 0b01) | ((value & 1) << 1));
      channel.size = static_cast<Channel::Size>((value >> 2) & 1);
      channel.time = static_cast<Channel::Timing>((value >> 4) & 3);
      channel.repeat  = (value & 2) && channel.time != Channel::Immediate;
      channel.gamepak = (value & 8) && chan_id == 3;
      channel.interrupt = value & 64;
      channel.enable = value & 128;

      OnChannelWritten(channel, enable_old);
      break;
    }
  }

  while (IsRunning()) {
    Run();
  }
}

void DMA::OnChannelWritten(Channel& channel, bool enable_old) {
  // If the DMA is enabled this information will be regenerated below.
  hblank_set.set(channel.id, false);
  vblank_set.set(channel.id, false);
  video_set.set(channel.id, false);

  if (!channel.enable) {
    runnable_set.set(channel.id, false);

    // Handle disabling the DMA before it started up.
    // TODO: not exactly known how hardware handles this edge-case.
    if (channel.startup_event != nullptr) {
      LOG_WARN("DMA{0} was cancelled before its startup completed.", channel.id);
      scheduler.Cancel(channel.startup_event);
      channel.startup_event = nullptr;
    }

    // Handle DMA channel self-disable (via writing to its control register).
    // TODO: not exactly known how hardware handles this edge-case.
    if (channel.id == active_dma_id) {
      LOG_WARN("DMA{0} triggered self-disable!", channel.id);
      early_exit_trigger = true;
      SelectNextDMA();
    }
    return;
  }

  /* Update H-blank/V-blank DMA sets.
   * This information is used to schedule these DMAs on request.
   */
  switch (channel.time) {
    case Channel::HBlank:
      hblank_set.set(channel.id, true);
      break;
    case Channel::VBlank:
      vblank_set.set(channel.id, true);
      break;
    case Channel::Special:
      if (channel.id == 3) {
        video_set.set(3, true);
      }
      break;
  }

  if (enable_old) {
    return;
  }

  int src_page = GetUnaliasedMemoryArea(channel.src_addr >> 24);

  channel.latch.dst_addr = channel.dst_addr;
  channel.latch.src_addr = channel.src_addr;

  if (src_page == 0x08) {
    channel.src_cntl = Channel::Control::Increment;
  }

  if (channel.time == Channel::Special && (channel.id == 1 || channel.id == 2)) {
    channel.is_fifo_dma = true;

    channel.size = Channel::Size::Word;
    channel.latch.length = 4;
    channel.latch.src_addr &= ~3;
    channel.latch.dst_addr &= ~3;
  } else {
    channel.is_fifo_dma = false;

    auto mask = channel.size == Channel::Word ? ~3 : ~1;
    channel.latch.src_addr &= mask;
    channel.latch.dst_addr &= mask;
    channel.latch.length = channel.length & g_dma_len_mask[channel.id];
    if (channel.latch.length == 0) {
      channel.latch.length = g_dma_len_mask[channel.id] + 1;
    }

    if (channel.time == Channel::Immediate) {
      ScheduleDMAs(1 << channel.id);
    }
  }
}

} // namespace nba::core
