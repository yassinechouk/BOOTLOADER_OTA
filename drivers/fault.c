#include <stdint.h>
#include "fault.h"

/* ----------------------------------------------------------------
 * Fault status registers -- ARM DDI0403 (ARMv7-M ARM) section B3.2.
 * ---------------------------------------------------------------- */
#define SCB_CFSR    (*(volatile uint32_t *)0xE000ED28UL)  /* configurable    */
#define SCB_HFSR    (*(volatile uint32_t *)0xE000ED2CUL)  /* hard fault      */
#define SCB_MMFAR   (*(volatile uint32_t *)0xE000ED34UL)  /* mem-manage addr */
#define SCB_BFAR    (*(volatile uint32_t *)0xE000ED38UL)  /* bus fault addr  */

/* CFSR is three bytes in one word: MMFSR[7:0], BFSR[15:8], UFSR[31:16] */
#define CFSR_MMARVALID  (1U << 7)
#define CFSR_BFARVALID  (1U << 15)

/* The UFSR bits worth naming. NOCP fires when an FP instruction runs
   with the FPU still disabled -- the failure the CPACR write in
   startup.s exists to prevent. */
#define UFSR_UNDEFINSTR (1U << 16)
#define UFSR_INVSTATE   (1U << 17)
#define UFSR_INVPC      (1U << 18)
#define UFSR_NOCP       (1U << 19)
#define UFSR_UNALIGNED  (1U << 24)
#define UFSR_DIVBYZERO  (1U << 25)


static void fault_puts(const char *s)
{
    while (*s != '\0') {
        fault_putc(*s++);
    }
}

static void fault_hex32(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    fault_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        fault_putc(digits[(v >> i) & 0xFU]);
    }
}

static void fault_line(const char *label, uint32_t v)
{
    fault_puts(label);
    fault_hex32(v);
    fault_puts("\r\n");
}


/*
 * Called with a pointer to the stack frame the processor pushed on
 * exception entry. Its layout is fixed by the architecture:
 *
 *     [0] R0   [1] R1   [2] R2   [3] R3
 *     [4] R12  [5] LR   [6] PC   [7] xPSR
 *
 * PC is the instruction that faulted, and the most useful number here:
 * arm-none-eabi-addr2line against the .elf turns it into a source line.
 */
void fault_report(uint32_t *frame, const char *which)
{
    uint32_t cfsr = SCB_CFSR;

    fault_puts("\r\n\r\n!!! ");
    fault_puts(which);
    fault_puts(" !!!\r\n");

    fault_line("  PC   : ", frame[6]);
    fault_line("  LR   : ", frame[5]);
    fault_line("  xPSR : ", frame[7]);
    fault_line("  R0   : ", frame[0]);
    fault_line("  R1   : ", frame[1]);
    fault_line("  R2   : ", frame[2]);
    fault_line("  R3   : ", frame[3]);
    fault_line("  R12  : ", frame[4]);

    fault_line("  CFSR : ", cfsr);
    fault_line("  HFSR : ", SCB_HFSR);

    /* The fault address registers only hold meaning when their VALID
       bit is set; printing them unconditionally invites chasing a
       stale address from an earlier fault. */
    if (cfsr & CFSR_MMARVALID) {
        fault_line("  MMFAR: ", SCB_MMFAR);
    }
    if (cfsr & CFSR_BFARVALID) {
        fault_line("  BFAR : ", SCB_BFAR);
    }

    if (cfsr & UFSR_NOCP) {
        fault_puts("  cause: coprocessor disabled (FPU not enabled?)\r\n");
    }
    if (cfsr & UFSR_UNALIGNED) {
        fault_puts("  cause: unaligned access\r\n");
    }
    if (cfsr & UFSR_DIVBYZERO) {
        fault_puts("  cause: divide by zero\r\n");
    }
    if (cfsr & UFSR_UNDEFINSTR) {
        fault_puts("  cause: undefined instruction\r\n");
    }
    if (cfsr & UFSR_INVSTATE) {
        fault_puts("  cause: invalid state (Thumb bit clear?)\r\n");
    }
    if (cfsr & UFSR_INVPC) {
        fault_puts("  cause: invalid PC on exception return\r\n");
    }

    fault_puts("  halted; watchdog will reset\r\n");

    /* Deliberately no iwdg_feed() from here on. */
    for (;;) {
    }
}

void fault_hard(uint32_t *frame)  { fault_report(frame, "HARD FAULT"); }
void fault_mem(uint32_t *frame)   { fault_report(frame, "MEMMANAGE FAULT"); }
void fault_bus(uint32_t *frame)   { fault_report(frame, "BUS FAULT"); }
void fault_usage(uint32_t *frame) { fault_report(frame, "USAGE FAULT"); }

/*
 * Naked so that no prologue disturbs the stack before the frame
 * pointer is captured.
 *
 * Bit 2 of EXC_RETURN (in LR on entry) selects which stack was in use
 * when the exception was taken: clear means MSP, set means PSP. Both
 * images run everything on MSP today, but reading it correctly costs
 * three instructions and stops this becoming wrong the day an RTOS
 * appears.
 *
 * r0 holds the frame pointer on entry to the C trampoline, which is
 * where AAPCS expects the first argument, so the branch needs no
 * further setup. Branching rather than calling keeps the faulting
 * frame as the only thing on the stack.
 */
#define FAULT_ENTRY(name, target)                         \
    __attribute__((naked)) void name(void)                \
    {                                                     \
        __asm__ volatile (                                \
            "tst   lr, #4   \n"                           \
            "ite   eq       \n"                           \
            "mrseq r0, msp  \n"                           \
            "mrsne r0, psp  \n"                           \
            "b     " #target "\n"                         \
        );                                                \
    }

FAULT_ENTRY(HardFault_Handler,  fault_hard)
FAULT_ENTRY(MemManage_Handler,  fault_mem)
FAULT_ENTRY(BusFault_Handler,   fault_bus)
FAULT_ENTRY(UsageFault_Handler, fault_usage)
