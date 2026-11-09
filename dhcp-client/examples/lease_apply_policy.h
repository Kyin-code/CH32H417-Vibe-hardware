/*
 * lease_apply_policy.h -- decide whether a bound/renewed lease actually
 * requires reprogramming the network stack.
 *
 * This lives outside v5f_bringup.c on purpose: it is the piece that used to
 * cause a reboot loop ("bound -> reboot -> INIT-REBOOT -> ACK -> renewed ->
 * reboot -> ..."). Keeping it as pure logic lets the host test suite prove
 * that a steady state lease never triggers a second reboot.
 *
 * It depends only on dhcp_client.h, so it is testable on a PC.
 */
#ifndef LEASE_APPLY_POLICY_H
#define LEASE_APPLY_POLICY_H

#include "dhcp_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the network stack is currently programmed with. */
typedef struct {
    uint32_t address;
    uint32_t subnet_mask;
    uint32_t router;
    uint8_t valid; /* 0 => the stack is still unconfigured */
} applied_netcfg_t;

typedef enum {
    LEASE_APPLY_NONE = 0, /* identical to what is already applied     */
    LEASE_APPLY_INITIAL,  /* stack not configured yet                 */
    LEASE_APPLY_CHANGED   /* address/mask/gateway differ              */
} lease_apply_action_t;

/** What should be done with `lease` given what is already applied? */
lease_apply_action_t lease_apply_decide(const applied_netcfg_t *applied,
                                       const dhcp_lease_t *lease);

/** Record that `lease` is now programmed into the stack (idempotent). */
void lease_apply_commit(applied_netcfg_t *applied, const dhcp_lease_t *lease);

/** Mark the stack as unconfigured (e.g. after ETH_LibInit with 0.0.0.0). */
void lease_apply_reset(applied_netcfg_t *applied);

#ifdef __cplusplus
}
#endif

#endif /* LEASE_APPLY_POLICY_H */
