#ifndef UPRINT_H
#define UPRINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the debug UART (115200 8N1). */
void uprint_init(void);

/* String; '\n' is expanded to CRLF. */
void uprint_str(const char *s);

/* Unsigned decimal / hex / dotted-quad IPv4 (host order). */
void uprint_u32(uint32_t v);
void uprint_hex32(uint32_t v);
void uprint_hex8(uint32_t v);
void uprint_ip(uint32_t ip);

#ifdef __cplusplus
}
#endif

#endif /* UPRINT_H */
