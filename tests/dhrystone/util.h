// Bare-metal "util.h" shim for the RPM core model.
//
// The riscv-tests Dhrystone sources (tests/riscv-tests/benchmarks/dhrystone/
// dhrystone.c / dhrystone_main.c) include <util.h>, normally provided by the
// riscv-tests benchmarks/common/ runtime. That common util.h pulls in the whole
// riscv-tests common environment (crt.S + syscalls.c + encoding.h + the proxy-
// kernel/HTIF syscall layer), which targets a richer machine than this model.
//
// Instead we reuse the lightweight CoreMark barebones runtime already wired up
// for this model (coremark/barebones: crt0.S, link.ld, ee_printf.c driving the
// HTIF "tohost" console). This shim supplies just the three symbols the
// Dhrystone sources expect from <util.h>, mapped onto that runtime. The build
// puts this directory ahead of the submodule on the include path so this header
// is used instead of benchmarks/common/util.h.

#ifndef _RPM_DHRY_UTIL_H
#define _RPM_DHRY_UTIL_H

// Inline CSR read. dhrystone.h's __riscv timer branch uses read_csr(mcycle).
// We read mcycle (machine cycle, 0xB00) rather than the user 'cycle' CSR, which
// traps without mcounteren/privilege setup on this model.
#define read_csr(reg)                                                          \
    ({                                                                         \
        unsigned long __tmp;                                                   \
        __asm__ volatile("csrr %0, " #reg : "=r"(__tmp));                      \
        __tmp;                                                                 \
    })

// riscv-tests uses setStats() to bracket the measured region for its stat
// harness. We have no such harness; timing is done directly via read_csr(mcycle)
// in Start_Timer()/Stop_Timer(), so this is a no-op.
static inline void
setStats(int enable)
{
    (void)enable;
}

// Console output. The barebones ee_printf.c (shared with the CoreMark port)
// implements ee_printf(), which sends bytes through the HTIF "tohost" byte that
// the model routes to stdout. Map the Dhrystone printf() calls onto it.
extern int ee_printf(const char *fmt, ...);
#define printf ee_printf

#endif /* _RPM_DHRY_UTIL_H */
