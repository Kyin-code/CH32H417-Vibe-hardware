/*
 * test_policy.c -- lease application policy (the piece that used to make the
 * V5F example reboot forever).
 *
 * The example's apply hook now calls lease_apply_decide() before doing
 * anything; these tests prove the decision table and, more importantly, that a
 * steady state lease can only ever cause one reboot.
 */
#include "test_support.h"
#include "lease_apply_policy.h"

static void mk_lease(dhcp_lease_t *l, uint32_t addr, uint32_t mask, uint32_t gw)
{
    memset(l, 0, sizeof(*l));
    l->address = addr;
    l->subnet_mask = mask;
    l->router = gw;
    l->server_id = 0xC0A80101u;
    l->lease_time_s = 3600u;
    l->t1_s = 1800u;
    l->t2_s = 3150u;
    l->valid = 1u;
}

static void test_initial_then_stable(void)
{
    applied_netcfg_t applied;
    dhcp_lease_t lease;

    lease_apply_reset(&applied);
    mk_lease(&lease, 0xC0A80164u, 0xFFFFFF00u, 0xC0A80101u);

    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_INITIAL);
    lease_apply_commit(&applied, &lease);

    /* Second and third time round: nothing to do. */
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_NONE);
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_NONE);
    CHECK_U32(applied.valid, 1u);
}

static void test_changed_fields(void)
{
    applied_netcfg_t applied;
    dhcp_lease_t lease;

    lease_apply_reset(&applied);
    mk_lease(&lease, 0xC0A80164u, 0xFFFFFF00u, 0xC0A80101u);
    lease_apply_commit(&applied, &lease);

    /* different address */
    lease.address = 0xC0A80165u;
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_CHANGED);

    /* different mask, same address */
    lease.address = 0xC0A80164u;
    lease.subnet_mask = 0xFFFF0000u;
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_CHANGED);

    /* different gateway */
    lease.subnet_mask = 0xFFFFFF00u;
    lease.router = 0xC0A801FEu;
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_CHANGED);

    /* DNS-only change must not force a stack reconfiguration */
    lease.router = 0xC0A80101u;
    lease.dns[0] = 0x01010101u;
    lease.dns_count = 1u;
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_NONE);
}

static void test_invalid_lease(void)
{
    applied_netcfg_t applied;
    dhcp_lease_t lease;

    lease_apply_reset(&applied);
    mk_lease(&lease, 0xC0A80164u, 0xFFFFFF00u, 0xC0A80101u);

    lease.valid = 0u;
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_NONE);

    lease.valid = 1u;
    lease.address = 0u;
    CHECK_U32(lease_apply_decide(&applied, &lease), LEASE_APPLY_NONE);

    /* NULL arguments are tolerated */
    CHECK_U32(lease_apply_decide(0, &lease), LEASE_APPLY_NONE);
    CHECK_U32(lease_apply_decide(&applied, 0), LEASE_APPLY_NONE);
    lease_apply_commit(0, &lease);
    lease_apply_reset(0);
}

/*
 * Reboot loop regression test.
 *
 * Boot 1: stack unconfigured -> BOUND -> apply -> reboot  (#1)
 * Boot 2: stack programmed from .noinit -> INIT-REBOOT -> ACK (renewed event)
 *         -> decide() must return NONE, otherwise we reboot forever.
 * Then 20 renewals with the same lease, and finally one real address change.
 */
static void test_no_reboot_loop(void)
{
    applied_netcfg_t applied; /* stands in for the .noinit copy */
    dhcp_lease_t lease;
    unsigned reboots = 0u;
    unsigned i;

    lease_apply_reset(&applied);
    mk_lease(&lease, 0xC0A80164u, 0xFFFFFF00u, 0xC0A80101u);

    /* boot 1: BOUND */
    if (lease_apply_decide(&applied, &lease) != LEASE_APPLY_NONE) {
        reboots++;
        lease_apply_commit(&applied, &lease); /* persisted in .noinit */
    }
    CHECK_U32(reboots, 1u);

    /* boot 2 .. boot 21: the INIT-REBOOT ACK and every renewal report the same
     * lease and must not trigger another reboot (applied survives reboot). */
    for (i = 0; i < 20u; i++) {
        if (lease_apply_decide(&applied, &lease) != LEASE_APPLY_NONE) {
            reboots++;
            lease_apply_commit(&applied, &lease);
        }
    }
    CHECK_U32(reboots, 1u);

    /* a genuinely different lease costs exactly one more reboot */
    lease.address = 0xC0A80177u;
    if (lease_apply_decide(&applied, &lease) != LEASE_APPLY_NONE) {
        reboots++;
        lease_apply_commit(&applied, &lease);
    }
    CHECK_U32(reboots, 2u);
    if (lease_apply_decide(&applied, &lease) != LEASE_APPLY_NONE) {
        reboots++;
        lease_apply_commit(&applied, &lease);
    }
    CHECK_U32(reboots, 2u);
}

void test_policy_run_all(void)
{
    test_initial_then_stable();
    test_changed_fields();
    test_invalid_lease();
    test_no_reboot_loop();
}
