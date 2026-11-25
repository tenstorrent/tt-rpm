# RV32 bare-metal runtime

Startup code and linker script for bare-metal RISC-V test programs; `tests/accel` builds
against them.

`crt.S`, `encoding.h` and `test.ld` are copied from the
[riscv-tests](https://github.com/riscv-software-src/riscv-tests) project and modified.
They are distributed under its BSD-3-Clause license, reproduced in [LICENSE](LICENSE);
their "See LICENSE for license details." headers refer to that file, so they should not
be given SPDX headers. Among the changes, `crt.S` defines `tohost`/`fromhost` in `.data`
and does not set `mstatus.VS`, and `test.ld` has no `PHDRS` block.

`tests/Makefile` doesn't use them; it builds RV64 workloads (`-march=rv64gc -mabi=lp64d`)
with its own startup code.
