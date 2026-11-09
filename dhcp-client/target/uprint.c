/*
 * uprint.c -- minimal UART output, to avoid dragging in newlib's printf.
 *
 * Why: linking `printf` pulls libc_a-vfprintf.o (9.5 KB) plus the soft-float
 * helpers adddf3/subdf3 (8.7 KB each) once %f support gets referenced. On this
 * board the V3F flash window is only 64 KB and the vendor demo fits in 46 KB,
 * so those 27 KB are the difference between booting and "region FLASH
 * overflowed".
 *
 * The API is deliberately tiny: strings, unsigned decimals, hex. Everything
 * the DHCP client needs to report.
 */
#include "uprint.h"

#include "ch32h417.h"
#include "debug.h"

#ifndef UPRINT_BAUD
#define UPRINT_BAUD 115200u
#endif

static void uputc(char c)
{
    /* Same UART the vendor debug.c uses: UART1 on this board (COM3 on the
     * WCH-LinkE side, PB15/PA8 through the CH32V305 bridge). */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {
    }
    USART_SendData(USART1, (uint8_t)c);
}

void uprint_init(void)
{
    USART_Printf_Init(UPRINT_BAUD);
}

void uprint_str(const char *s)
{
    while (s != 0 && *s != '\0') {
        if (*s == '\n') {
            uputc('\r');
        }
        uputc(*s++);
    }
}

void uprint_u32(uint32_t v)
{
    char buf[11];
    int i = 0;

    if (v == 0u) {
        uputc('0');
        return;
    }
    while (v > 0u && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        uputc(buf[--i]);
    }
}

void uprint_hex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    int shift;

    uputc('0');
    uputc('x');
    for (shift = 28; shift >= 0; shift -= 4) {
        uputc(hex[(v >> shift) & 0xFu]);
    }
}

void uprint_hex8(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";

    uputc(hex[(v >> 4) & 0xFu]);
    uputc(hex[v & 0xFu]);
}

void uprint_ip(uint32_t ip)
{
    uprint_u32((ip >> 24) & 0xFFu);
    uputc('.');
    uprint_u32((ip >> 16) & 0xFFu);
    uputc('.');
    uprint_u32((ip >> 8) & 0xFFu);
    uputc('.');
    uprint_u32(ip & 0xFFu);
}
