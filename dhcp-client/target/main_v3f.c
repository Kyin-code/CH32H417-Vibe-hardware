/*
 * main_v3f.c -- CH32H417 V3F helper core.
 *
 * The nanoCH32H417 board runs two cores. The V5F application lives at
 * 0x00010000 and does not start by itself: the V3F core has to wake it up and
 * then park in STOP mode. This mirrors the vendor flow in
 * doc/EVT/EXAM/ETH/DHCP/V3F/User/main.c with Run_Core == Run_Core_V5F.
 *
 * Flash layout of the merged image (burned at 0x08000000):
 *   0x00000000 .. 0x0000FFFF  V3F (this file, 64K window)
 *   0x00010000 ..             V5F (main_v5f.c, the DHCP application)
 *
 * printf goes to UART1 because Core_V3F selects DEBUG_UART1 in debug.h.
 */
#include "debug.h"
#include "ch32h417.h"
#include "system_ch32h417.h"
#include "core_riscv.h"

int main(void)
{
    SystemInit();
    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);
    Delay_Ms(300);

    printf("\r\n\r\n=== ch32h417 dual-core helper (V3F) ===\r\n");
    printf("SystemClk=%u CoreClk=%u\r\n", (unsigned)SystemClock, (unsigned)SystemCoreClock);

    /* Hand the V5F core its start address (0x00010000) and let it run. */
    NVIC_WakeUp_V5F(Core_V5F_StartAddr);
    printf("V5F woken at 0x%08x, V3F parking\r\n", (unsigned)Core_V5F_StartAddr);
    Delay_Ms(50);

    PWR_EnterSTOPMode(PWR_Regulator_ON, PWR_STOPEntry_WFE);

    for (;;) {
        /* Only reached if something wakes the V3F core again. */
        __WFI();
    }
}
