// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

// DMA doorbell microbench for the loose-coupling accelerator path (cluster run).
//
// The core fills a src buffer, writes one memcpy descriptor into the guest-memory
// descriptor ring, rings the device's MMIO doorbell (an uncacheable store: the dcache
// bypass forwards it to the fabric, the cluster cache peels it to the device's ring
// front-end), spins while the DmaEngine copies, then verifies the destination.
//
// Exit via HTIF tohost: 0 = dst matches src, 1 = mismatch, 3 = trap.
//
// Self-contained: pairs with tests/rv32/crt.S (expects _init / handle_trap / tohost) and
// tests/rv32/test.ld — no newlib.

#include <stdint.h>

// Defined by tests/rv32/crt.S (.data, 64B-aligned).
extern volatile uint64_t tohost;
extern volatile uint64_t fromhost;

static void __attribute__((noreturn)) tohost_exit(uint64_t code) {
    tohost = (code << 1) | 1;
    for (;;)
        ;
}

uintptr_t handle_trap(uintptr_t mcause, uintptr_t mepc, uintptr_t sp) {
    (void)mcause;
    (void)mepc;
    (void)sp;
    tohost_exit(3);
}

// Must match the cluster config's accelerators[] section (bar/ring) and the core
// config's dcache uncacheable range (bar).
#define BAR_BASE 0xF0000000ULL
#define RING_BASE 0x81000000ULL
#define SRC_ADDR 0x81100000ULL
#define DST_ADDR 0x81200000ULL
#define COPY_LEN 128ULL  // 2 src lines + 2 dst lines at 64B

static int main(void) {
    volatile uint64_t* const ring = (volatile uint64_t*)RING_BASE;
    volatile uint64_t* const doorbell = (volatile uint64_t*)BAR_BASE;
    volatile uint8_t* const src = (volatile uint8_t*)SRC_ADDR;
    volatile uint8_t* const dst = (volatile uint8_t*)DST_ADDR;

    for (uint64_t i = 0; i < COPY_LEN; ++i) {
        src[i] = (uint8_t)(0x40 + i);
        dst[i] = 0;
    }

    // One descriptor: {opcode=memcpy(1), src, dst, len} (32B, matches RingFrontEnd).
    ring[0] = 1;
    ring[1] = SRC_ADDR;
    ring[2] = DST_ADDR;
    ring[3] = COPY_LEN;
    __sync_synchronize();

    *doorbell = 1;  // new tail: one descriptor ready
    __sync_synchronize();

    // No completion queue yet — spin long enough for the copy to land.
    for (volatile uint32_t i = 0; i < 200000; ++i)
        ;

    uint64_t fail = 0;
    for (uint64_t i = 0; i < COPY_LEN; ++i)
        if (dst[i] != (uint8_t)(0x40 + i)) fail = 1;
    return (int)fail;
}

void __attribute__((noreturn)) _init(void) { tohost_exit((uint64_t)main()); }
