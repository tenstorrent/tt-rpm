# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

# Build the DMA doorbell microbench and run it on the cluster model with a
# DmaEngine attached (loose coupling: MMIO doorbell + descriptor ring).
#
#   make          build dma_doorbell.elf
#   make run      build + run the cluster smoke test
#   make clean
#
# Needs a riscv64 gcc (riscv64-unknown-elf-gcc by default; -nostdlib, so a linux-gnu one also works).

HERE  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT  := $(HERE)/../..
# riscv-tests benchmark runtime (crt.S defines tohost, calls _init/handle_trap); xlen-generic.
RT    := $(HERE)/../rv32
# CC needs an origin guard: make predefines CC=cc, so a plain ?= would never apply.
ifeq ($(origin CC),default)
CC := riscv64-unknown-elf-gcc
endif
CLUSTER ?= $(ROOT)/build/top/cluster/cluster

# rv64imac/lp64 matches tests/accel/whisper.json (zicsr is explicit for newer binutils);
# medany reaches 0x80000000.
CFLAGS := -O2 -g -ffreestanding -nostdlib -nostartfiles -mcmodel=medany \
          -march=rv64imac_zicsr -mabi=lp64 -I$(RT) -T $(RT)/test.ld

all: dma_doorbell.elf

dma_doorbell.elf: dma_doorbell.c $(RT)/crt.S $(RT)/test.ld
	$(CC) $(CFLAGS) $(RT)/crt.S dma_doorbell.c -o $@

run: dma_doorbell.elf
	cd $(HERE) && $(CLUSTER) --cluster-config config.yaml

clean:
	rm -f dma_doorbell.elf

.PHONY: all run clean
