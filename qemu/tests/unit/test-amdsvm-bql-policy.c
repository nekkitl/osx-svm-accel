#include "qemu/osdep.h"
#include "../../accel/amdsvm/amdsvm-bql-policy.h"

typedef struct MockLock {
    uint32_t busy_attempts;
    uint32_t attempts;
    uint32_t relaxes;
} MockLock;

static bool mock_try_acquire(void *opaque)
{
    MockLock *mock = opaque;

    mock->attempts++;
    return mock->attempts > mock->busy_attempts;
}

static void mock_relax(void *opaque)
{
    MockLock *mock = opaque;

    mock->relaxes++;
}

static void test_bql_spin_uncontended(void)
{
    MockLock mock = { 0 };
    AMDVMBQLSpinResult result = amdsvm_bql_try_acquire_bounded(
        64, mock_try_acquire, mock_relax, &mock);

    g_assert_true(result.acquired);
    g_assert_cmpuint(result.attempts, ==, 1);
    g_assert_cmpuint(mock.relaxes, ==, 0);
}

static void test_bql_spin_acquires_after_contention(void)
{
    MockLock mock = { .busy_attempts = 7 };
    AMDVMBQLSpinResult result = amdsvm_bql_try_acquire_bounded(
        64, mock_try_acquire, mock_relax, &mock);

    g_assert_true(result.acquired);
    g_assert_cmpuint(result.attempts, ==, 8);
    g_assert_cmpuint(mock.relaxes, ==, 7);
}

static void test_bql_spin_budget_falls_back(void)
{
    MockLock mock = { .busy_attempts = UINT32_MAX };
    AMDVMBQLSpinResult result = amdsvm_bql_try_acquire_bounded(
        32, mock_try_acquire, mock_relax, &mock);

    g_assert_false(result.acquired);
    g_assert_cmpuint(result.attempts, ==, 32);
    g_assert_cmpuint(mock.relaxes, ==, 31);
}

static void test_bql_spin_disabled(void)
{
    MockLock mock = { .busy_attempts = 2 };
    AMDVMBQLSpinResult result = amdsvm_bql_try_acquire_bounded(
        0, mock_try_acquire, mock_relax, &mock);

    g_assert_false(result.acquired);
    g_assert_cmpuint(result.attempts, ==, 0);
    g_assert_cmpuint(mock.attempts, ==, 0);
    g_assert_cmpuint(mock.relaxes, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/bql/uncontended", test_bql_spin_uncontended);
    g_test_add_func("/amdsvm/bql/contention",
                    test_bql_spin_acquires_after_contention);
    g_test_add_func("/amdsvm/bql/budget-fallback",
                    test_bql_spin_budget_falls_back);
    g_test_add_func("/amdsvm/bql/disabled", test_bql_spin_disabled);
    return g_test_run();
}
