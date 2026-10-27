/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rbtest.h - the airtime tests, fed from the sample's callbacks.
 */
#ifndef RBTEST_H_
#define RBTEST_H_

#include <rb9704/rb9704.h>

void rbtest_mo_done(uint16_t topic, uint16_t id, int result);
void rbtest_mt(const struct rb9704_mt *mt);

#endif /* RBTEST_H_ */
