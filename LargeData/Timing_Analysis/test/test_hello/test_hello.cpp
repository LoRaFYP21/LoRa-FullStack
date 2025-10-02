#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

void test_hello(void) {
    TEST_ASSERT_EQUAL(1, 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hello);
    return UNITY_END();
}