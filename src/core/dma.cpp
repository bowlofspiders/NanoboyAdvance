/*
 * Copyright (C) 2018 Frederic Meyer. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cpu.hpp"
#include "dma.hpp"

using namespace ARM;
using namespace NanoboyAdvance::GBA;

static constexpr std::uint32_t g_dma_dst_mask[4] = { 0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF };
static constexpr std::uint32_t g_dma_src_mask[4] = { 0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF };
static constexpr std::uint32_t g_dma_len_mask[4] = { 0x3FFF, 0x3FFF, 0x3FFF, 0xFFFF };

/* TODO: what happens if src_cntl equals DMA_RELOAD? */
static constexpr int g_dma_modify[2][4] = {
    { 2, -2, 0, 2 },
    { 4, -4, 0, 4 }
};

/* Retrieves DMA with highest priority from a DMA bitset. */
static constexpr int g_dma_from_bitset[] = {
    /* 0b0000 */ -1,
    /* 0b0001 */  0,
    /* 0b0010 */  1,
    /* 0b0011 */  0,
    /* 0b0100 */  2,
    /* 0b0101 */  0,
    /* 0b0110 */  1,
    /* 0b0111 */  0,
    /* 0b1000 */  3,
    /* 0b1001 */  0,
    /* 0b1010 */  1,
    /* 0b1011 */  0,
    /* 0b1100 */  2,
    /* 0b1101 */  0,
    /* 0b1110 */  1,
    /* 0b1111 */  0
};

void DMAController::Reset() {
    hblank_set = 0;
    vblank_set = 0;
    run_set = 0;
    current = 0;
    interleaved = false;
    
    for (int id = 0; id < 4; id++) {
        dma[id].enable = false;
        dma[id].repeat = false;
        dma[id].interrupt = false;
        dma[id].gamepak  = false;
        dma[id].length   = 0;
        dma[id].dst_addr = 0;
        dma[id].src_addr = 0;
        dma[id].internal.length   = 0;
        dma[id].internal.dst_addr = 0;
        dma[id].internal.src_addr = 0;
        dma[id].size     = DMA_HWORD;
        dma[id].time     = DMA_IMMEDIATE;
        dma[id].dst_cntl = DMA_INCREMENT;
        dma[id].src_cntl = DMA_INCREMENT;
    }
}

auto DMAController::Read(int id, int offset) -> std::uint8_t {
    /* TODO: are SAD/DAD/CNT_L readable? */
    switch (offset) {
        /* DMAXCNT_H */
        case 10: {
            return (dma[id].dst_cntl << 5) |
                   (dma[id].src_cntl << 7);
        }
        case 11: {
            return (dma[id].src_cntl >> 1) |
                   (dma[id].size     << 2) |
                   (dma[id].time     << 4) |
                   (dma[id].repeat    ? 2   : 0) |
                   (dma[id].gamepak   ? 8   : 0) |
                   (dma[id].interrupt ? 64  : 0) |
                   (dma[id].enable    ? 128 : 0);
        }
        default: return 0;
    }
}

void DMAController::Write(int id, int offset, std::uint8_t value) {
    switch (offset) {
        /* DMAXSAD */
        case 0: {
            dma[id].src_addr = (dma[id].src_addr & 0xFFFFFF00) | (value<<0 );
            break;
        }
        case 1: {
            dma[id].src_addr = (dma[id].src_addr & 0xFFFF00FF) | (value<<8 );
            break;
        }
        case 2: {
            dma[id].src_addr = (dma[id].src_addr & 0xFF00FFFF) | (value<<16);
            break;
        }
        case 3: {
            dma[id].src_addr = (dma[id].src_addr & 0x00FFFFFF) | (value<<24);
            break;
        }

        /* DMAXDAD */
        case 4: {
            dma[id].dst_addr = (dma[id].dst_addr & 0xFFFFFF00) | (value<<0 );
            break;
        }
        case 5: {
            dma[id].dst_addr = (dma[id].dst_addr & 0xFFFF00FF) | (value<<8 );
            break;
        }
        case 6: {
            dma[id].dst_addr = (dma[id].dst_addr & 0xFF00FFFF) | (value<<16);
            break;
        }
        case 7: {
            dma[id].dst_addr = (dma[id].dst_addr & 0x00FFFFFF) | (value<<24);
            break;
        }

        /* DMAXCNT_L */
        case 8: {
            dma[id].length = (dma[id].length & 0xFF00) | (value<<0);
            break;
        }
        case 9: {
            dma[id].length = (dma[id].length & 0x00FF) | (value<<8);
            break;
        }

        /* DMAXCNT_H */
        case 10: {
            dma[id].dst_cntl = DMAControl((value >> 5) & 3);
            dma[id].src_cntl = DMAControl((dma[id].src_cntl & 0b10) | (value>>7));
            break;
        }
        case 11: {
            bool enable_previous = dma[id].enable;

            dma[id].src_cntl  = DMAControl((dma[id].src_cntl & 0b01) | ((value & 1)<<1));
            dma[id].size      = DMASize((value>>2) & 1);
            dma[id].time      = DMATime((value>>4) & 3);
            dma[id].repeat    = value & 2;
            dma[id].gamepak   = value & 8;
            dma[id].interrupt = value & 64;
            dma[id].enable    = value & 128;

            /* Update HBLANK/VBLANK DMA sets. */
            if (dma[id].time == DMA_HBLANK) {
                hblank_set |=  (1<<id);
                vblank_set &= ~(1<<id);
            } else if (dma[id].time == DMA_VBLANK) { 
                hblank_set &= ~(1<<id);
                vblank_set |=  (1<<id);
            } else {
                hblank_set &= ~(1<<id);
                vblank_set &= ~(1<<id);
            }
                
            /* DMA state is latched on "rising" enable bit. */
            if (!enable_previous && dma[id].enable) {
                /* Latch sanitized values into internal DMA state. */
                dma[id].internal.dst_addr = dma[id].dst_addr & g_dma_dst_mask[id];
                dma[id].internal.src_addr = dma[id].src_addr & g_dma_src_mask[id];
                dma[id].internal.length   = dma[id].length   & g_dma_len_mask[id];

                if (dma[id].internal.length == 0) {
                    dma[id].internal.length = g_dma_len_mask[id] + 1;
                }

                /* Schedule DMA if is setup for immediate execution. */
                if (dma[id].time == DMA_IMMEDIATE) {
                    MarkDMAForExecution(id);
                }
            }
            break;
        }
    }
}

void DMAController::MarkDMAForExecution(int id) {
    /* If no other DMA is running or this DMA has higher priority
     * then execute this DMA directly.
     * Lower priority DMAs will be interleaved in the latter case.
     */
    if (run_set == 0) {
        current = id;
    } else if (id < current) {
        current = id;
        interleaved = true;
    }

    /* Mark DMA as running. */
    run_set |= (1 << id);
}

void DMAController::TriggerHBlankDMA() {
    int hblank_dma = g_dma_from_bitset[run_set & hblank_set];
    
    if (hblank_dma >= 0)
        MarkDMAForExecution(hblank_dma);
}

void DMAController::TriggerVBlankDMA() {
    int vblank_dma = g_dma_from_bitset[run_set & vblank_set];
    
    if (vblank_dma >= 0)
        MarkDMAForExecution(vblank_dma);
}

void DMAController::Run() {
    auto& dma = this->dma[current];
    
    auto src_cntl = dma.src_cntl;
    auto dst_cntl = dma.dst_cntl;
    int src_modify = g_dma_modify[dma.size][src_cntl];
    int dst_modify = g_dma_modify[dma.size][dst_cntl];
    
    std::uint32_t word;
    
    /* Run DMA until completion or interruption. */
    switch (dma.size) {
        case DMA_WORD: {
            while (dma.internal.length != 0) {
                if (cpu->run_until <= 0) return;

                /* Stop if DMA was interleaved by higher priority DMA. */
                if (interleaved) {
                    interleaved = false;
                    return;
                }

                word = cpu->ReadWord(dma.internal.src_addr, ACCESS_SEQ);
                cpu->WriteWord(dma.internal.dst_addr, word, ACCESS_SEQ);

                dma.internal.src_addr += src_modify;
                dma.internal.dst_addr += dst_modify;
                dma.internal.length--;
            }
            break;
        }
        case DMA_HWORD: {
            while (dma.internal.length != 0) {
                if (cpu->run_until <= 0) return;

                /* Stop if DMA was interleaved by higher priority DMA. */
                if (interleaved) {
                    interleaved = false;
                    return;
                }

                word = cpu->ReadHalf(dma.internal.src_addr, ACCESS_SEQ);
                cpu->WriteHalf(dma.internal.dst_addr, word, ACCESS_SEQ);

                dma.internal.src_addr += src_modify;
                dma.internal.dst_addr += dst_modify;
                dma.internal.length--;
            }
            break;
        }
    }
    
    /* NOTE: if this code path is reached, the DMA has completed. */
    
    if (dma.interrupt) {
        cpu->mmio.irq_if |= CPU::INT_DMA0 << current;
    }
    
    if (dma.repeat) {
        /* Reload the internal length counter. */
        dma.internal.length = dma.length & g_dma_len_mask[current];
        if (dma.internal.length == 0) {
            dma.internal.length = g_dma_len_mask[current] + 1;
        }

        /* Reload destination address if specified. */
        if (dst_cntl == DMA_RELOAD) {
            dma.internal.dst_addr = dma.dst_addr & g_dma_dst_mask[current];
        }

        /* If DMA is specified to be non-immediate, wait for it to be retriggered. */
        if (dma.time != DMA_IMMEDIATE) {
            run_set &= ~(1 << current);
        }
    } else {
        dma.enable = false;
        run_set &= ~(1 << current);
    }
    
    if (run_set > 0) {
        current = g_dma_from_bitset[run_set];
    }
}