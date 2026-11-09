/*
 * main_min.c -- minimal CH32H417 V5F bring-up test.
 *
 * Purpose: prove toolchain + linker script + flash + UART8 chain works before
 * adding the Ethernet stack. printf goes to UART8 (DEBUG_UART8 for Core_V5F).
 */
#include "debug.h"
#include "ch32h417.h"
#include "system_ch32h417.h"

int main(void)
{
    unsigned n = 0u;

    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);

    printf("\r\n\r\n=== dhcp-client target: V5F boot OK ===\r\n");
    printf("SystemClk=%u\r\n", (unsigned)SystemCoreClock);

    for (;;) {
        n++;
        printf("alive %u\r\n", n);
        Delay_Ms(1000);
    }
}
