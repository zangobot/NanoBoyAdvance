/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include "cpu.hpp"

#include <common/likely.hpp>
#include <cstring>

namespace nba::core {

constexpr int CPU::s_ws_nseq[4];
constexpr int CPU::s_ws_seq0[2];
constexpr int CPU::s_ws_seq1[2];
constexpr int CPU::s_ws_seq2[2];

using Key = InputDevice::Key;

CPU::CPU(std::shared_ptr<Config> config)
    : ARM7TDMI::ARM7TDMI(scheduler, this)
    , config(config)
    , irq(*this, scheduler)
    , dma(*this, irq, scheduler)
    , apu(scheduler, dma, config)
    , ppu(scheduler, irq, dma, config)
    , timer(scheduler, irq, apu)
    , serial_bus(irq) {
  std::memset(memory.bios, 0, 0x04000);
  memory.rom.size = 0;
  memory.rom.mask = 0;
  Reset();
}

void CPU::Reset() {
  std::memset(memory.wram, 0, 0x40000);
  std::memset(memory.iram, 0, 0x08000);

  mmio = {};
  prefetch = {};
  bus_is_controlled_by_dma = false;
  openbus_from_dma = false;
  UpdateMemoryDelayTable();

  for (int i = 16; i < 256; i++) {
    cycles16[int(Access::Nonsequential)][i] = 1;
    cycles32[int(Access::Nonsequential)][i] = 1;
    cycles16[int(Access::Sequential)][i] = 1;
    cycles32[int(Access::Sequential)][i] = 1;
  }

  scheduler.Reset();
  irq.Reset();
  dma.Reset();
  timer.Reset();
  apu.Reset();
  ppu.Reset();
  serial_bus.Reset();
  ARM7TDMI::Reset();

  if (config->skip_bios) {
    SwitchMode(arm::MODE_SYS);
    state.bank[arm::BANK_SVC][arm::BANK_R13] = 0x03007FE0;
    state.bank[arm::BANK_IRQ][arm::BANK_R13] = 0x03007FA0;
    state.r13 = 0x03007F00;
    state.r15 = 0x08000000;
  }

  m4a_soundinfo = nullptr;
  m4a_original_freq = 0;
  if (config->audio.m4a_xq_enable && memory.rom.data != nullptr) {
    M4ASearchForSampleFreqSet();
  }

  config->input_dev->SetOnChangeCallback(std::bind(&CPU::OnKeyPress,this));
}

void CPU::Tick(int cycles) {
  // TODO: get rid of this terrible, terrible hack.
  ppu.ResetReadingVRAM();

  openbus_from_dma = false;
  
  if (unlikely(dma.IsRunning() && !bus_is_controlled_by_dma)) {
    bus_is_controlled_by_dma = true;
    dma.Run();
    bus_is_controlled_by_dma = false;
    openbus_from_dma = true;
  }

  scheduler.AddCycles(cycles);

  if (prefetch.active && !bus_is_controlled_by_dma) {
    prefetch.countdown -= cycles;

    if (prefetch.countdown <= 0) {
      prefetch.count++;
      prefetch.active = false;
    }
  }
}

void CPU::Idle() {
  PrefetchStepRAM(1);
}

void CPU::PrefetchStepRAM(int cycles) {
  // TODO: bypass prefetch RAM step during DMA?
  if (unlikely(!mmio.waitcnt.prefetch)) {
    Tick(cycles);
    return;
  }

  auto thumb = state.cpsr.f.thumb;
  auto r15 = state.r15;

  /* During any execute cycle except for the fetch cycle, 
   * r15 will be three instruction ahead instead of two.
   */
  if (!code) {
    r15 -= thumb ? 2 : 4;
  }

  if (!prefetch.active && prefetch.rom_code_access && prefetch.count < prefetch.capacity) {
    if (prefetch.count == 0) {
      if (thumb) {
        prefetch.opcode_width = 2;
        prefetch.capacity = 8;
        prefetch.duty = cycles16[int(Access::Sequential)][r15 >> 24];
      } else {
        prefetch.opcode_width = 4;
        prefetch.capacity = 4;
        prefetch.duty = cycles32[int(Access::Sequential)][r15 >> 24];
      }
      prefetch.last_address = r15 + prefetch.opcode_width;
      prefetch.head_address = prefetch.last_address;
    } else {
      prefetch.last_address += prefetch.opcode_width;
    }

    prefetch.countdown = prefetch.duty;
    prefetch.active = true;
  }

  Tick(cycles);
}

void CPU::PrefetchStepROM(std::uint32_t address, int cycles) {
  // TODO: bypass prefetch ROM step during DMA?
  if (unlikely(!mmio.waitcnt.prefetch)) {
    Tick(cycles);
    return;
  }

  prefetch.rom_code_access = code;

  if (prefetch.active) {
    if (code && address == prefetch.last_address) {
      // Complete the load and consume the fetched (half)word right away.
      Tick(prefetch.countdown);
      prefetch.count--;
      return;
    }

    prefetch.active = false;
  }

  if (code && prefetch.count != 0) {
    if (address == prefetch.head_address) {
      prefetch.count--;
      prefetch.head_address += prefetch.opcode_width;
      PrefetchStepRAM(1);
      return;
    } else {
      prefetch.count = 0;
    }
  }

  Tick(cycles);
}

void CPU::RunFor(int cycles) {
  bool m4a_xq_enable = config->audio.m4a_xq_enable && m4a_setfreq_address != 0;
  if (m4a_xq_enable && m4a_soundinfo != nullptr) {
    M4AFixupPercussiveChannels();
  }

  auto limit = scheduler.GetTimestampNow() + cycles;

  while (scheduler.GetTimestampNow() < limit) {
    if (unlikely(mmio.haltcnt == HaltControl::HALT && irq.HasServableIRQ())) {
      mmio.haltcnt = HaltControl::RUN;
    }

    if (likely(mmio.haltcnt == HaltControl::RUN)) {
      if (unlikely(m4a_xq_enable && state.r15 == m4a_setfreq_address)) {
        M4ASampleFreqSetHook();
      }
      Run();
    } else {
      Tick(scheduler.GetRemainingCycleCount());
    }
  }
}

void CPU::UpdateMemoryDelayTable() {
  auto cycles16_n = cycles16[int(Access::Nonsequential)];
  auto cycles16_s = cycles16[int(Access::Sequential)];
  auto cycles32_n = cycles32[int(Access::Nonsequential)];
  auto cycles32_s = cycles32[int(Access::Sequential)];

  int sram_cycles = 1 + s_ws_nseq[mmio.waitcnt.sram];

  cycles16_n[0xE] = sram_cycles;
  cycles32_n[0xE] = sram_cycles;
  cycles16_s[0xE] = sram_cycles;
  cycles32_s[0xE] = sram_cycles;

  for (int i = 0; i < 2; i++) {
    /* ROM: WS0/WS1/WS2 non-sequential timing. */
    cycles16_n[0x8 + i] = 1 + s_ws_nseq[mmio.waitcnt.ws0_n];
    cycles16_n[0xA + i] = 1 + s_ws_nseq[mmio.waitcnt.ws1_n];
    cycles16_n[0xC + i] = 1 + s_ws_nseq[mmio.waitcnt.ws2_n];

    /* ROM: WS0/WS1/WS2 sequential timing. */
    cycles16_s[0x8 + i] = 1 + s_ws_seq0[mmio.waitcnt.ws0_s];
    cycles16_s[0xA + i] = 1 + s_ws_seq1[mmio.waitcnt.ws1_s];
    cycles16_s[0xC + i] = 1 + s_ws_seq2[mmio.waitcnt.ws2_s];

    /* ROM: WS0/WS1/WS2 32-bit non-sequential access: 1N access, 1S access */
    cycles32_n[0x8 + i] = cycles16_n[0x8] + cycles16_s[0x8];
    cycles32_n[0xA + i] = cycles16_n[0xA] + cycles16_s[0xA];
    cycles32_n[0xC + i] = cycles16_n[0xC] + cycles16_s[0xC];

    /* ROM: WS0/WS1/WS2 32-bit sequential access: 2S accesses */
    cycles32_s[0x8 + i] = cycles16_s[0x8] * 2;
    cycles32_s[0xA + i] = cycles16_s[0xA] * 2;
    cycles32_s[0xC + i] = cycles16_s[0xC] * 2;
  }
}

void CPU::M4ASearchForSampleFreqSet() {
  static const std::uint8_t pattern[] = {
    0x53, 0x6D, 0x73, 0x68, 0x70, 0xB5, 0x02, 0x1C,
    0x1E, 0x48, 0x04, 0x68, 0xF0, 0x20, 0x00, 0x03,
    0x10, 0x40, 0x02, 0x0C
  };
  for (std::uint32_t i = 0; i < memory.rom.size; i++) {
    bool match = true;
    for (int j = 0; j < sizeof(pattern); j++) {
      if (memory.rom.data[i + j] != pattern[j]) {
        match = false;
        i += j;
        break;
      }
    }
    if (match) {
      m4a_setfreq_address = i + 0x08000008;
      LOG_INFO("Found M4A SetSampleFreq() routine at 0x{0:08X}.", m4a_setfreq_address);
      return;
    }
  }
}

void CPU::M4ASampleFreqSetHook() {
  static const int frequency_tab[16] = {
    0, 5734, 7884, 10512,
    13379, 15768, 18157, 21024,
    26758, 31536, 36314, 40137,
    42048, 0, 0, 0
  };

  LOG_INFO("M4A SampleFreqSet() called: r0 = 0x{0:08X}", state.r0);

  m4a_original_freq = frequency_tab[(state.r0 >> 16) & 15];
  state.r0 = 0x00090000;
  m4a_soundinfo = nullptr;

  std::uint32_t soundinfo_p1 = Read<std::uint32_t>(memory.rom.data.get(), (m4a_setfreq_address & 0x00FFFFFF) + 492);
  std::uint32_t soundinfo_p2;
  LOG_INFO("M4A SoundInfo pointer at 0x{0:08X}", soundinfo_p1);

  switch (soundinfo_p1 >> 24) {
    case REGION_EWRAM:
      soundinfo_p2 = Read<std::uint32_t>(memory.wram, soundinfo_p1 & 0x00FFFFFF);
      break;
    case REGION_IWRAM:
      soundinfo_p2 = Read<std::uint32_t>(memory.iram, soundinfo_p1 & 0x00FFFFFF);
      break;
    default:
      LOG_ERROR("M4A SoundInfo pointer is outside of IWRAM or EWRAM, unsupported.");
      return;
  }

  LOG_INFO("M4A SoundInfo address is 0x{0:08X}", soundinfo_p2);

  switch (soundinfo_p2 >> 24) {
    case REGION_EWRAM:
      m4a_soundinfo = reinterpret_cast<M4ASoundInfo*>(memory.wram + (soundinfo_p2 & 0x00FFFFFF));
      break;
    case REGION_IWRAM:
      m4a_soundinfo = reinterpret_cast<M4ASoundInfo*>(memory.iram + (soundinfo_p2 & 0x00FFFFFF));
      break;
    default:
      LOG_ERROR("M4A SoundInfo is outside of IWRAM or EWRAM, unsupported.");
      return;
  }
}

void CPU::M4AFixupPercussiveChannels() {
  for (int i = 0; i < kM4AMaxDirectSoundChannels; i++) {
    if (m4a_soundinfo->channels[i].type == 8) {
      m4a_soundinfo->channels[i].type = 0;
      m4a_soundinfo->channels[i].freq = m4a_original_freq;
    }
  }
}


void CPU::OnKeyPress() {
  auto &keyinput = mmio.keyinput;
  // cache keystate into keyinput
  keyinput = (config->input_dev->Poll(Key::A)      ? 0 : 1)  |
             (config->input_dev->Poll(Key::B)      ? 0 : 2)  |
             (config->input_dev->Poll(Key::Select) ? 0 : 4)  |
             (config->input_dev->Poll(Key::Start)  ? 0 : 8)  |
             (config->input_dev->Poll(Key::Right)  ? 0 : 16) |
             (config->input_dev->Poll(Key::Left)   ? 0 : 32) |
             (config->input_dev->Poll(Key::Up)     ? 0 : 64) |
             (config->input_dev->Poll(Key::Down)   ? 0 : 128) |
             (config->input_dev->Poll(Key::R) ? 0 : 256) |
             (config->input_dev->Poll(Key::L) ? 0 : 512);

  CheckKeypadInterrupt();
}

void CPU::CheckKeypadInterrupt() {
  const auto& keycnt = mmio.keycnt;
  const auto keyinput = ~mmio.keyinput & 0x3FF;
  if (!keycnt.interrupt)
    return;
  if (keycnt.and_mode) {
    if (keycnt.input_mask == keyinput)
      irq.Raise(IRQ::Source::Keypad);
  } else if ((keycnt.input_mask & keyinput) != 0) {
    irq.Raise(IRQ::Source::Keypad);
  }
}

} // namespace nba::core
