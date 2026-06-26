# SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

# Build bare-metal CoreMark/Dhrystone for the RPM core model and run them.
#
# No kernel exists, so workloads are built bare-metal (crt0 + linker script at
# 0x80000000); timing via mcycle, console + exit via HTIF "tohost".
#
# Targets:
#   make all            build both ELFs (no model run)   [default]
#   make coremark       build CoreMark ELF
#   make dhrystone      build Dhrystone ELF
#   make run_coremark   build + run CoreMark in the model
#   make run_dhrystone  build + run Dhrystone in the model
#   make clean          remove build/
#
# Overridable vars: CC, OBJDUMP, CORE, CONFIG, ITERATIONS, RUN_ICOUNT
# Needs the bare-metal toolchain (riscv64-unknown-elf-gcc; see install-toolchain-conda.sh).

HERE     := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
CM       := $(HERE)/coremark
DHRY_SRC := $(HERE)/riscv-tests/benchmarks/dhrystone
DHRY     := $(HERE)/dhrystone
RT       := $(CM)/barebones
BUILD    := $(HERE)/build
# DHRY_SRC: Dhrystone sources (submodule); DHRY: port shim (util.h);
# RT: shared runtime (crt0, link.ld, console); BUILD: output dir for ELFs.

# CORE: core model binary; CONFIG: Sparta config (-c); RUN_ICOUNT: -i value (0 = run until tohost).
# CC needs an origin guard: make predefines CC=cc, so a plain ?= would never apply.
ifeq ($(origin CC),default)
CC := riscv64-unknown-elf-gcc
endif
OBJDUMP    ?= riscv64-unknown-elf-objdump
CORE       ?= $(HERE)/../build/core/core
CONFIG     ?= $(HERE)/../build/core/config.yaml
ITERATIONS ?= 20
RUN_ICOUNT ?= 0

# rv64gc/lp64d matches the toolchain multilib (libgcc/newlib); FP is never executed
# in these int-only workloads. -nostartfiles uses our crt0; medany reaches 0x80000000.
# The whisper.json ISA (tests/whisper.json) must cover what these binaries contain (imac).
COMMON_CFLAGS := -O2 -g -ffreestanding -nostartfiles -mcmodel=medany \
                 -march=rv64gc -mabi=lp64d -T $(RT)/link.ld

CM_ELF   := $(BUILD)/coremark.bare.elf
DHRY_ELF := $(BUILD)/dhrystone.bare.elf
WHISPER  := $(BUILD)/whisper.json

# Upstream CoreMark has no bare-metal runtime for this model. Our additions
# (barebones/crt0.S, barebones/link.ld) and the modified barebones port files
# (core_portme.c/.h, ee_printf.c) live as a patch over the submodule so a fresh
# checkout can be reproduced. See coremark-baremetal.patch.
PATCH := $(HERE)/coremark-baremetal.patch

CM_SRCS := $(RT)/crt0.S \
           $(CM)/core_list_join.c $(CM)/core_main.c $(CM)/core_matrix.c \
           $(CM)/core_state.c $(CM)/core_util.c \
           $(RT)/core_portme.c $(RT)/ee_printf.c $(RT)/cvt.c

# check_cc: fail early with guidance if the bare-metal RISC-V compiler is missing.
# Invoked at the top of each compile recipe, so it only fires when a build is
# actually needed -- an already-built ELF still runs (run_*) without the toolchain.
define check_cc
command -v $(CC) >/dev/null 2>&1 || { \
    echo "error: RISC-V cross compiler '$(CC)' not found in PATH." >&2; \
    echo "       The tests need the bare-metal (newlib) toolchain riscv64-unknown-elf-gcc;" >&2; \
    echo "       a glibc '*-linux-gnu-gcc' will not work." >&2; \
    echo "       Install it (one-time) and activate the env, then retry:" >&2; \
    echo "         bash $(HERE)/install-toolchain-conda.sh   # creates conda env 'riscv'" >&2; \
    echo "         conda activate riscv" >&2; \
    echo "       Or point CC at an existing toolchain:" >&2; \
    echo "         make -C tests CC=/path/to/riscv64-unknown-elf-gcc" >&2; \
    exit 1; }
endef

.PHONY: all coremark dhrystone run_coremark run_dhrystone clean help patch

all: coremark dhrystone

coremark:  $(CM_ELF)
dhrystone: $(DHRY_ELF)

# Materialize the bare-metal runtime from the patch. A single `git apply` creates
# crt0.S + link.ld and patches core_portme.c/.h + ee_printf.c in the submodule.
# These two files don't exist upstream, so they're the build's natural trigger:
# make runs this only when they're missing (e.g. a fresh submodule checkout). The
# guard keeps it a no-op (idempotent) if crt0.S is already present.
$(RT)/crt0.S $(RT)/link.ld:
	@if [ ! -f $(RT)/crt0.S ]; then \
	    test -f $(PATCH) || { echo "error: missing $(PATCH)" >&2; exit 1; }; \
	    echo "==> Applying bare-metal patch to coremark submodule"; \
	    git -C $(CM) apply $(PATCH); \
	fi

$(BUILD):
	mkdir -p $(BUILD)

# The model reads whisper.json from the ELF's directory; copy the shared config in
# (copy, not symlink, to avoid self-referential link loops).
$(WHISPER): $(HERE)/whisper.json | $(BUILD)
	cp -f $< $@

$(CM_ELF): $(CM_SRCS) $(RT)/link.ld | $(BUILD) $(WHISPER)
	@$(call check_cc)
	@echo "==> Building bare-metal CoreMark (ITERATIONS=$(ITERATIONS))"
	$(CC) $(COMMON_CFLAGS) -I$(CM) -I$(RT) \
	    -DITERATIONS=$(ITERATIONS) -DPERFORMANCE_RUN=1 -DHAS_FLOAT=0 \
	    -DFLAGS_STR='"-O2 -g -march=rv64gc -mabi=lp64d barebones (int-only)"' \
	    $(CM_SRCS) -o $@ -lgcc

# DHRY_CFLAGS: Dhrystone is pre-ANSI K&R C. -std=gnu11 keeps the legacy meaning
# of empty parens `()` (unspecified args), which C23 (the GCC 15+ default) would
# otherwise treat as `(void)` and reject K&R calls. GCC 14+ also promotes
# implicit-int, implicit-function-declaration, int-conversion and
# incompatible-pointer-types to hard errors; downgrade them so the source builds.
# -I$(DHRY) first so our util.h shim wins over the submodule's common util.h.
# Submodule sources are checked in-recipe (not as prereqs) for a friendly message.
DHRY_CFLAGS := -std=gnu11 \
               -Wno-error=implicit-int -Wno-error=implicit-function-declaration \
               -Wno-error=int-conversion -Wno-error=incompatible-pointer-types
$(DHRY_ELF): $(RT)/crt0.S $(RT)/ee_printf.c $(RT)/link.ld $(DHRY)/util.h | $(BUILD) $(WHISPER)
	@$(call check_cc)
	@test -f $(DHRY_SRC)/dhrystone_main.c || { \
	    echo "error: riscv-tests submodule not initialized ($(DHRY_SRC) missing)" >&2; \
	    echo "       run: git submodule update --init tests/riscv-tests" >&2; exit 1; }
	@echo "==> Building bare-metal Dhrystone (NUMBER_OF_RUNS=1000)"
	$(CC) $(COMMON_CFLAGS) $(DHRY_CFLAGS) \
	    -I$(DHRY) -I$(DHRY_SRC) -I$(CM) -I$(RT) -DHAS_FLOAT=0 \
	    $(RT)/crt0.S $(DHRY_SRC)/dhrystone.c $(DHRY_SRC)/dhrystone_main.c $(RT)/ee_printf.c \
	    -o $@ -lgcc

run_coremark: $(CM_ELF)
	@$(call run_model,$(CM_ELF),CoreMark)

run_dhrystone: $(DHRY_ELF)
	@$(call run_model,$(DHRY_ELF),Dhrystone)

# run_model(elf, name): sanity-check the ELF, then run it in the core model with the
# Sparta config. A bare-metal build must contain no ecall (syscall) or it stalls.
# Expanded as one shell command (backslash-joined) so it works as a single recipe line.
define run_model
echo "==> Built $(1)"; file $(1); \
if command -v $(OBJDUMP) >/dev/null 2>&1 && $(OBJDUMP) -d $(1) | grep -qw ecall; then \
    echo "warning: $(1) contains ecall instructions (will stall the model)" >&2; fi; \
if [ ! -x $(CORE) ]; then \
    echo "error: core model not found at $(CORE) (build it first)" >&2; exit 1; fi; \
if [ ! -f $(CONFIG) ]; then \
    echo "error: Sparta config not found at $(CONFIG)" >&2; exit 1; fi; \
echo "==> Running $(2): $(CORE) -c $(CONFIG) -i $(RUN_ICOUNT) --target-elf $(1)"; \
$(CORE) -c $(CONFIG) -i $(RUN_ICOUNT) --target-elf $(1)
endef

clean:
	rm -rf $(BUILD)

help:
	@echo "Targets: all coremark dhrystone run_coremark run_dhrystone clean"
