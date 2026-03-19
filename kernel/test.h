/*
 * test.h — Kernel self-test framework.
 */

#ifndef TEST_H
#define TEST_H

/* Run all kernel self-tests.  Prints results to console.
 * Returns the number of failed tests (0 = all passed). */
int kernel_run_tests(void);

#endif /* TEST_H */
