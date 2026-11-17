#include "qemu/osdep.h"
#include "../../accel/amdsvm/amdsvm-native-cr3-policy.h"

static void test_native_cr3_policy(void)
{
    g_assert_true(amdsvm_native_cr3_policy_safe(48, 48, 0));
    g_assert_true(amdsvm_native_cr3_policy_safe(52, 52, 0));
    g_assert_false(amdsvm_native_cr3_policy_safe(48, 52, 0));
    g_assert_false(amdsvm_native_cr3_policy_safe(52, 48, 0));
    g_assert_false(amdsvm_native_cr3_policy_safe(48, 48,
                                                  AMDSVM_CR4_PCIDE));
    g_assert_false(amdsvm_native_cr3_policy_safe(0, 0, 0));
    g_assert_false(amdsvm_native_cr3_policy_safe(31, 31, 0));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/cr3/native-policy", test_native_cr3_policy);
    return g_test_run();
}
