.syntax unified
.cpu cortex-m4
.fpu fpv4-sp-d16
.thumb

/* =========================================================
   RESET HANDLER -- first code executed after reset
   ========================================================= */
    .section  .text.Reset_Handler
    .weak     Reset_Handler
    .type     Reset_Handler, %function

Reset_Handler:
    /* 1. Load the stack pointer.
          The processor has already done this from the vector table;
          this line guarantees a correct state if we arrive here by
          any means other than a reset. */
    ldr   sp, =_estack

    /* 2. Enable the FPU.
          The Cortex-M4F leaves CP10/CP11 disabled out of reset. This
          image is built with -mfloat-abi=hard, so the compiler is
          free to emit VFP instructions and to pass floats in VFP
          registers; the first such instruction executed with the
          coprocessor disabled raises a UsageFault (NOCP).

          No float appears in either image today, so the omission was
          invisible -- the build worked because nothing exercised it,
          not because it was correct. In a bootloader that failure
          mode is expensive: the fault lands in Default_Handler, the
          watchdog resets the board, boot_fail_count advances, and a
          perfectly good image is rolled back three boots later for a
          reason nothing reports.

          CPACR is at 0xE000ED88; bits 23:20 grant full access to
          CP11 and CP10. DSB then ISB because the effect must be in
          place before the next instruction is fetched. */
    ldr   r0, =0xE000ED88
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)
    str   r1, [r0]
    dsb
    isb

    /* 3. Copy .data from FLASH to RAM.
          Initial values of global variables are stored in flash;
          their execution location is in RAM. */
    movs  r1, #0
    b     LoopCopyDataInit

CopyDataInit:
    ldr   r3, =_sidata          /* source address in flash    */
    ldr   r3, [r3, r1]          /* read a 32-bit word         */
    str   r3, [r0, r1]          /* write it to RAM            */
    adds  r1, r1, #4

LoopCopyDataInit:
    ldr   r0, =_sdata
    ldr   r3, =_edata
    adds  r2, r0, r1
    cmp   r2, r3
    bcc   CopyDataInit

    /* 4. Zero-fill .bss.
          The C standard guarantees that uninitialised globals are
          zero at startup. */
    ldr   r2, =_sbss
    b     LoopFillZerobss

FillZerobss:
    movs  r3, #0
    str   r3, [r2], #4

LoopFillZerobss:
    ldr   r3, =_ebss
    cmp   r2, r3
    bcc   FillZerobss

    /* 5. Call main() */
    bl    main

    /* main() must never return in an embedded system */
LoopForever:
    b     LoopForever

.size Reset_Handler, .-Reset_Handler


/* =========================================================
   DEFAULT HANDLER

   Every vector table entry must point somewhere. Unimplemented
   ones land here. In debug, a program stuck in Infinite_Loop
   signals an unhandled interrupt -- often a HardFault.
   ========================================================= */
    .section  .text.Default_Handler,"ax",%progbits

Default_Handler:
Infinite_Loop:
    b  Infinite_Loop

.size Default_Handler, .-Default_Handler


/* =========================================================
   VECTOR TABLE

   Array of addresses placed at the very start of flash. The
   processor indexes it based on the event: at reset it reads
   the first two entries, at an interrupt the one matching its
   number.

   The first 16 entries are the system exceptions defined by
   ARM, common to all Cortex-M. The peripheral interrupts
   specific to the STM32L4 follow.
   ========================================================= */
    .section  .isr_vector,"a",%progbits
    .type     g_pfnVectors, %object

g_pfnVectors:
    /* --- ARM system exceptions --- */
    .word _estack                       /* 0x00  initial stack pointer */
    .word Reset_Handler                 /* 0x04  reset                 */
    .word NMI_Handler                   /* 0x08 */
    .word HardFault_Handler             /* 0x0C */
    .word MemManage_Handler             /* 0x10 */
    .word BusFault_Handler              /* 0x14 */
    .word UsageFault_Handler            /* 0x18 */
    .word 0                             /* 0x1C  reserved */
    .word 0                             /* 0x20  reserved */
    .word 0                             /* 0x24  reserved */
    .word 0                             /* 0x28  reserved */
    .word SVC_Handler                   /* 0x2C */
    .word DebugMon_Handler              /* 0x30 */
    .word 0                             /* 0x34  reserved */
    .word PendSV_Handler                /* 0x38 */
    .word SysTick_Handler               /* 0x3C */

    /* --- STM32L4 peripheral interrupts --- */
    .word WWDG_IRQHandler                     /* IRQ  0 */
    .word PVD_PVM_IRQHandler                  /* IRQ  1 */
    .word TAMP_STAMP_IRQHandler               /* IRQ  2 */
    .word RTC_WKUP_IRQHandler                 /* IRQ  3 */
    .word FLASH_IRQHandler                    /* IRQ  4 */
    .word RCC_IRQHandler                      /* IRQ  5 */
    .word EXTI0_IRQHandler                    /* IRQ  6 */
    .word EXTI1_IRQHandler                    /* IRQ  7 */
    .word EXTI2_IRQHandler                    /* IRQ  8 */
    .word EXTI3_IRQHandler                    /* IRQ  9 */
    .word EXTI4_IRQHandler                    /* IRQ 10 */
    .word DMA1_Channel1_IRQHandler            /* IRQ 11 */
    .word DMA1_Channel2_IRQHandler            /* IRQ 12 */
    .word DMA1_Channel3_IRQHandler            /* IRQ 13 */
    .word DMA1_Channel4_IRQHandler            /* IRQ 14 */
    .word DMA1_Channel5_IRQHandler            /* IRQ 15 */
    .word DMA1_Channel6_IRQHandler            /* IRQ 16 */
    .word DMA1_Channel7_IRQHandler            /* IRQ 17 */
    .word ADC1_2_IRQHandler                   /* IRQ 18 */
    .word CAN1_TX_IRQHandler                  /* IRQ 19 */
    .word CAN1_RX0_IRQHandler                 /* IRQ 20 */
    .word CAN1_RX1_IRQHandler                 /* IRQ 21 */
    .word CAN1_SCE_IRQHandler                 /* IRQ 22 */
    .word EXTI9_5_IRQHandler                  /* IRQ 23 */
    .word TIM1_BRK_TIM15_IRQHandler           /* IRQ 24 */
    .word TIM1_UP_TIM16_IRQHandler            /* IRQ 25 */
    .word TIM1_TRG_COM_TIM17_IRQHandler       /* IRQ 26 */
    .word TIM1_CC_IRQHandler                  /* IRQ 27 */
    .word TIM2_IRQHandler                     /* IRQ 28 */
    .word TIM3_IRQHandler                     /* IRQ 29 */
    .word TIM4_IRQHandler                     /* IRQ 30 */
    .word I2C1_EV_IRQHandler                  /* IRQ 31 */
    .word I2C1_ER_IRQHandler                  /* IRQ 32 */
    .word I2C2_EV_IRQHandler                  /* IRQ 33 */
    .word I2C2_ER_IRQHandler                  /* IRQ 34 */
    .word SPI1_IRQHandler                     /* IRQ 35 */
    .word SPI2_IRQHandler                     /* IRQ 36 */
    .word USART1_IRQHandler                   /* IRQ 37 */
    .word USART2_IRQHandler                   /* IRQ 38  <- our handler */


/* =========================================================
   WEAK ALIASES

   Each handler points to Default_Handler by default. A STRONG
   definition of the same name elsewhere in the project -- for
   example USART2_IRQHandler in uart.c -- automatically replaces
   the alias: the linker always retains the strong definition.

   This is what allows implementing a handler without ever
   modifying this table.
   ========================================================= */

    .weak NMI_Handler
    .thumb_set NMI_Handler,Default_Handler

    .weak HardFault_Handler
    .thumb_set HardFault_Handler,Default_Handler

    .weak MemManage_Handler
    .thumb_set MemManage_Handler,Default_Handler

    .weak BusFault_Handler
    .thumb_set BusFault_Handler,Default_Handler

    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler,Default_Handler

    .weak SVC_Handler
    .thumb_set SVC_Handler,Default_Handler

    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler,Default_Handler

    .weak PendSV_Handler
    .thumb_set PendSV_Handler,Default_Handler

    .weak SysTick_Handler
    .thumb_set SysTick_Handler,Default_Handler

    .weak WWDG_IRQHandler
    .thumb_set WWDG_IRQHandler,Default_Handler

    .weak PVD_PVM_IRQHandler
    .thumb_set PVD_PVM_IRQHandler,Default_Handler

    .weak TAMP_STAMP_IRQHandler
    .thumb_set TAMP_STAMP_IRQHandler,Default_Handler

    .weak RTC_WKUP_IRQHandler
    .thumb_set RTC_WKUP_IRQHandler,Default_Handler

    .weak FLASH_IRQHandler
    .thumb_set FLASH_IRQHandler,Default_Handler

    .weak RCC_IRQHandler
    .thumb_set RCC_IRQHandler,Default_Handler

    .weak EXTI0_IRQHandler
    .thumb_set EXTI0_IRQHandler,Default_Handler

    .weak EXTI1_IRQHandler
    .thumb_set EXTI1_IRQHandler,Default_Handler

    .weak EXTI2_IRQHandler
    .thumb_set EXTI2_IRQHandler,Default_Handler

    .weak EXTI3_IRQHandler
    .thumb_set EXTI3_IRQHandler,Default_Handler

    .weak EXTI4_IRQHandler
    .thumb_set EXTI4_IRQHandler,Default_Handler

    .weak DMA1_Channel1_IRQHandler
    .thumb_set DMA1_Channel1_IRQHandler,Default_Handler

    .weak DMA1_Channel2_IRQHandler
    .thumb_set DMA1_Channel2_IRQHandler,Default_Handler

    .weak DMA1_Channel3_IRQHandler
    .thumb_set DMA1_Channel3_IRQHandler,Default_Handler

    .weak DMA1_Channel4_IRQHandler
    .thumb_set DMA1_Channel4_IRQHandler,Default_Handler

    .weak DMA1_Channel5_IRQHandler
    .thumb_set DMA1_Channel5_IRQHandler,Default_Handler

    .weak DMA1_Channel6_IRQHandler
    .thumb_set DMA1_Channel6_IRQHandler,Default_Handler

    .weak DMA1_Channel7_IRQHandler
    .thumb_set DMA1_Channel7_IRQHandler,Default_Handler

    .weak ADC1_2_IRQHandler
    .thumb_set ADC1_2_IRQHandler,Default_Handler

    .weak CAN1_TX_IRQHandler
    .thumb_set CAN1_TX_IRQHandler,Default_Handler

    .weak CAN1_RX0_IRQHandler
    .thumb_set CAN1_RX0_IRQHandler,Default_Handler

    .weak CAN1_RX1_IRQHandler
    .thumb_set CAN1_RX1_IRQHandler,Default_Handler

    .weak CAN1_SCE_IRQHandler
    .thumb_set CAN1_SCE_IRQHandler,Default_Handler

    .weak EXTI9_5_IRQHandler
    .thumb_set EXTI9_5_IRQHandler,Default_Handler

    .weak TIM1_BRK_TIM15_IRQHandler
    .thumb_set TIM1_BRK_TIM15_IRQHandler,Default_Handler

    .weak TIM1_UP_TIM16_IRQHandler
    .thumb_set TIM1_UP_TIM16_IRQHandler,Default_Handler

    .weak TIM1_TRG_COM_TIM17_IRQHandler
    .thumb_set TIM1_TRG_COM_TIM17_IRQHandler,Default_Handler

    .weak TIM1_CC_IRQHandler
    .thumb_set TIM1_CC_IRQHandler,Default_Handler

    .weak TIM2_IRQHandler
    .thumb_set TIM2_IRQHandler,Default_Handler

    .weak TIM3_IRQHandler
    .thumb_set TIM3_IRQHandler,Default_Handler

    .weak TIM4_IRQHandler
    .thumb_set TIM4_IRQHandler,Default_Handler

    .weak I2C1_EV_IRQHandler
    .thumb_set I2C1_EV_IRQHandler,Default_Handler

    .weak I2C1_ER_IRQHandler
    .thumb_set I2C1_ER_IRQHandler,Default_Handler

    .weak I2C2_EV_IRQHandler
    .thumb_set I2C2_EV_IRQHandler,Default_Handler

    .weak I2C2_ER_IRQHandler
    .thumb_set I2C2_ER_IRQHandler,Default_Handler

    .weak SPI1_IRQHandler
    .thumb_set SPI1_IRQHandler,Default_Handler

    .weak SPI2_IRQHandler
    .thumb_set SPI2_IRQHandler,Default_Handler

    .weak USART1_IRQHandler
    .thumb_set USART1_IRQHandler,Default_Handler

    .weak USART2_IRQHandler
    .thumb_set USART2_IRQHandler,Default_Handler
