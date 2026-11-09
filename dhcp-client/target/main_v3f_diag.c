/*
 * main_v3f_diag.c -- V3F-only diagnostic image.
 *
 * Purpose: answer ONE question -- does a freshly burned image actually run?
 *
 * It does nothing but initialise the clock and UART1, then print a banner
 * forever. No V5F wake-up, no WCHNET, no DHCP. So:
 *
 *   banner repeats  -> the flash write works; the problem is in the V5F
 *                      half of the real image (wake-up address, layout, ...)
 *   silence         -> the flash write itself is not landing (erase only),
 *                      or V3F dies before UART init
 */
#include "debug.h"
#include "ch32h417.h"
#include "system_ch32h417.h"

int main(void)
{
    unsigned n = 0u;

    SystemInit();
    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);

    for (;;) {
        n++;
        printf("\r\n*** V3F DIAG ALIVE n=%u clk=%u ***\r\n", n, (unsigned)SystemCoreClock);
        Delay_Ms(500);
    }
}
