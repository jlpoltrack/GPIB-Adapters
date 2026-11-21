/* CH32V407 startup, from the WCH repo's examples/common: no vendor SDK. Interrupts share one entry (trap.c,
 * mtvec mode 0); hardware stacking stays off (HW_POPDM_CTLR reset value), so handlers are plain C. */

    .section .init, "ax"
    .globl _start
_start:
    /* Reset fetches from the 0x00000000 flash alias (or the bootloader jumps here): go to the link address */
    .option push
    .option norelax
    .option norvc
    lui     t0, %hi(reset_handler)
    addi    t0, t0, %lo(reset_handler)
    jr      t0
    .option pop

    .text
reset_handler:
    csrci   mstatus, 8          /* MIE off: a bootloader may have left it on */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top

    la      a0, _sidata         /* .data from flash */
    la      a1, _sdata
    la      a2, _edata
1:  bgeu    a1, a2, 2f
    lw      t0, 0(a0)
    sw      t0, 0(a1)
    addi    a0, a0, 4
    addi    a1, a1, 4
    j       1b

2:  la      a0, _sbss           /* zero .bss */
    la      a1, _ebss
3:  bgeu    a0, a1, 4f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       3b

4:  li      t0, 1               /* CPU_RUN_CTLR: instruction prefetch on (RM 9.2.4.1) */
    csrw    0xBC0, t0
    la      t0, trap_entry      /* unified entry, mode 0 */
    csrw    mtvec, t0

    call    main
5:  j       5b
