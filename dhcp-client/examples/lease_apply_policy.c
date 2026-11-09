#include "lease_apply_policy.h"

#include <string.h>

lease_apply_action_t lease_apply_decide(const applied_netcfg_t *applied,
                                       const dhcp_lease_t *lease)
{
    if (applied == 0 || lease == 0) {
        return LEASE_APPLY_NONE;
    }
    if (lease->valid == 0u || lease->address == 0u) {
        return LEASE_APPLY_NONE;
    }
    if (applied->valid == 0u) {
        return LEASE_APPLY_INITIAL;
    }
    if (applied->address != lease->address || applied->subnet_mask != lease->subnet_mask ||
        applied->router != lease->router) {
        return LEASE_APPLY_CHANGED;
    }
    /* Same address, mask and gateway: nothing to reprogram. DNS changes do not
     * require a stack reconfiguration (they are handed to the resolver). */
    return LEASE_APPLY_NONE;
}

void lease_apply_commit(applied_netcfg_t *applied, const dhcp_lease_t *lease)
{
    if (applied == 0 || lease == 0) {
        return;
    }
    applied->address = lease->address;
    applied->subnet_mask = lease->subnet_mask;
    applied->router = lease->router;
    applied->valid = (lease->valid != 0u && lease->address != 0u) ? 1u : 0u;
}

void lease_apply_reset(applied_netcfg_t *applied)
{
    if (applied == 0) {
        return;
    }
    memset(applied, 0, sizeof(*applied));
}
