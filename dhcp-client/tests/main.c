/*
 * main.c -- test runner entry point.
 */
#include "test_support.h"

/* Test suite entry points are declared in test_support.h. */

int main(void)
{
    printf("dhcp-client unit tests\n");

    printf("[ suite ] packet / wire format\n");
    test_packet_run_all();
    printf("[ suite ] client state machine\n");
    test_client_run_all();
    printf("[ suite ] lease apply policy (V5F example logic)\n");
    test_policy_run_all();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
