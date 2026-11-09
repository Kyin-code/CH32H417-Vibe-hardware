/*
 * main_v5f_helper.c -- CH32H417 V5F core: HSEM handshake only.
 *
 * The Ethernet stack lives on V3F in this firmware (see main_v3f_eth.c). V5F
 * therefore does exactly what the vendor demo does in its
 * Run_Core_V3FandV5F branch: take/release the HSEM and idle. That keeps the
 * V5F image small (the vendor demo's V5F window is ~10 KB) which is what makes
 * it actually boot -- an 88 KB V5F image failed to start on real hardware.
 */
#include "debug.h"
#include "ch32h417.h"
#include "ch32h417_it.h"
#include "system_ch32h417.h"

int main(void)
{
    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);

    printf("V5F helper up\r\n");

    HSEM_FastTake(HSEM_ID0);
    HSEM_ReleaseOneSem(HSEM_ID0, 0);

    for (;;) {
        /* Park. The application runs on V3F. */
    }
}
