/*
 * _selfcheck/wchnet.h -- SYNTAX-CHECK STUB ONLY, NOT A SHIPPING HEADER.
 *
 * The real declarations live in
 *   nanoCH32H417-main/doc/EVT/EXAM/ETH/NetLib/wchnet.h
 * and are only usable with the WCH RISC-V toolchain, which is not installed in
 * this workspace. To keep the port honest we still type-check it on the host:
 *
 *   gcc -fsyntax-only -std=c99 -Wall -Wextra -Werror -pedantic \
 *       -Iinclude -Iports/ch32h417/_selfcheck ports/ch32h417/dhcp_port_wchnet.c
 *
 * The declarations below are copied from the vendor header (same names, same
 * parameter types, same field order) so a signature mismatch in the port is
 * caught here. This file must never be added to a target build's include path.
 */
#ifndef __WCHNET_H__
#define __WCHNET_H__

#include <stdint.h>

#define WCHNET_ERR_SUCCESS 0x00u
#define WCHNET_ERR_BUSY 0x10u
#define WCHNET_ERR_ARG 0x1au

#define PROTO_TYPE_IP_RAW 0u
#define PROTO_TYPE_UDP 2u
#define PROTO_TYPE_TCP 3u

#define SINT_STAT_RECV (1 << 2)
#define SINT_STAT_CONNECT (1 << 3)
#define SINT_STAT_DISCONNECT (1 << 4)

#define TCP_CLOSE_NORMAL 0u
#define TCP_CLOSE_ABANDON 2u

typedef struct _SOCK_INF {
    uint32_t IntStatus;      /* interrupt state                  */
    uint32_t SockIndex;      /* socket index value               */
    uint32_t RecvStartPoint; /* start pointer of the rx buffer   */
    uint32_t RecvBufLen;     /* rx buffer length                 */
    uint32_t RecvCurPoint;   /* current rx pointer               */
    uint32_t RecvReadPoint;  /* read pointer of the rx buffer    */
    uint32_t RecvRemLen;     /* remaining bytes                  */
    uint32_t ProtoType;      /* PROTO_TYPE_*                     */
    uint32_t SockStatus;     /* socket state                     */
    uint32_t DesPort;        /* destination port                 */
    uint32_t SourPort;       /* source port (protocol in IP_RAW) */
    uint8_t IPAddr[4];       /* destination IP                   */
    void *Resv1;
    void *Resv2;
    void *AppCallBack;
} SOCK_INF;

uint8_t WCHNET_SocketCreat(uint8_t *socketid, SOCK_INF *socinf);
uint8_t WCHNET_SocketSend(uint8_t socketid, uint8_t *buf, uint32_t *len);
uint8_t WCHNET_SocketRecv(uint8_t socketid, uint8_t *buf, uint32_t *len);
uint32_t WCHNET_SocketRecvLen(uint8_t socketid, uint32_t *bufaddr);
uint8_t WCHNET_SocketClose(uint8_t socketid, uint8_t mode);
uint8_t WCHNET_SocketUdpSendTo(uint8_t socketid, uint8_t *buf, uint32_t *slen, uint8_t *sip,
                               uint16_t port);
uint8_t WCHNET_GetSocketInt(uint8_t socketid);
void WCHNET_ModifyRecvBuf(uint8_t socketid, uint32_t bufaddr, uint32_t bufsize);

#endif /* __WCHNET_H__ */
