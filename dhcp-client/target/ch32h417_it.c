/*
 * ch32h417_it.c -- interrupt handlers for the dhcp-client target firmware.
 *
 * Own copy (not the vendor V5F/User/ch32h417_it.c) so that vendor sources stay
 * untouched and the DHCP tick can be injected.
 *
 * NOTE: the vendor code uses __attribute__((interrupt("WCH-Interrupt-fast"))),
 * which only exists in WCH's patched GCC. We build with the stock xPack
 * riscv-none-elf-gcc, so the standard "machine" interrupt attribute is used
 * instead: same mret + register save/restore semantics, only without WCH's
 * fast-stacking optimisation.
 */
#include "ch32h417_it.h"
#include "debug.h"
#include "ch32h417.h"
#include "uprint.h"

/* Provided by eth_driver.c, only used by the full firmware build. */
extern void ETH_PHYLink(void);
extern void WCHNET_ETHIsr(void);

volatile uint32_t g_tim2_ticks;

void NMI_Handler(void) __attribute__((interrupt("machine")));
void HardFault_Handler(void) __attribute__((interrupt("machine")));
void TIM2_IRQHandler(void) __attribute__((interrupt("machine")));
void ETH_IRQHandler(void) __attribute__((interrupt("machine")));
void EXTI15_8_IRQHandler(void) __attribute__((interrupt("machine")));

void NMI_Handler(void)
{
    while (1) {
    }
}

void HardFault_Handler(void)
{
    /* No printf here on purpose: pulling newlib's vfprintf costs ~9.5 KB out of
     * a 64 KB flash window. Use the tiny helper instead. */
    uprint_str("HardFault mepc=");
    uprint_hex32((uint32_t)__get_MEPC());
    uprint_str(" mcause=");
    uprint_hex32((uint32_t)__get_MCAUSE());
    uprint_str("\n");
    NVIC_SystemReset();
    while (1) {
    }
}

/* Provided by the application; weak so the minimal bring-up test still links. */
void app_tim2_tick(void) __attribute__((weak));

void TIM2_IRQHandler(void)
{
    g_tim2_ticks++;
    if (app_tim2_tick != 0) {
        app_tim2_tick();
    }
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
}

void ETH_IRQHandler(void)
{
#if !defined(Core_V5F)
    /* Ethernet lives on V3F in this firmware; V5F only runs the HSEM stub. */
    WCHNET_ETHIsr();
#endif
}

void EXTI15_8_IRQHandler(void)
{
#if !defined(Core_V5F)
    if (EXTI_GetITStatus(EXTI_Line13) != RESET) {
        ETH_PHYLink();
        EXTI_ClearITPendingBit(EXTI_Line13);
    }
#else
    EXTI_ClearITPendingBit(EXTI_Line13);
#endif
}
