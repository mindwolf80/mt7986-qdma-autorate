// SPDX-License-Identifier: GPL-2.0
/* Host self-test for ctl.c: cc -O2 -o ctl_test ctl_test.c ctl.c && ./ctl_test */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ctl.h"

static const struct ctl_cfg C = {
	.start = 300, .floor = 150, .ceil = 380, .hi_ms = 10, .lo_ms = 3, .dec = 0.95,
	.inc_frac = 0.01, .util = 0.8, .holdoff_ns = 300 * NS_MS, .base_ns = 30000 * NS_MS,
	.cur_ns = 200 * NS_MS, .qual_ns = 500 * NS_MS, .min_samples = 3,
	.fast_frac = 0, .target_frac = 0.97, .cooldown_ns = 0,	/* v1 behaviour */
};
/* v2 defaults */
static const struct ctl_cfg C2 = {
	.start = 300, .floor = 150, .ceil = 380, .hi_ms = 10, .lo_ms = 5, .dec = 0.95,
	.inc_frac = 0.002, .util = 0.8, .holdoff_ns = 300 * NS_MS, .base_ns = 30000 * NS_MS,
	.cur_ns = 100 * NS_MS, .qual_ns = 300 * NS_MS, .min_samples = 3,
	.fast_frac = 0.02, .target_frac = 0.97, .cooldown_ns = 400 * NS_MS,
	.bind_backlog_frac = 0,	/* v2 rules */
};

int main(void)
{
	static struct flow_win f;
	struct ctl_state s = { .rate = 300 };
	uint64_t t0 = 1000000 * NS_MS, t;
	double d, v[] = { 5, 1, 9, 3 };

	/* window: 20 ms baseline for 10 s, then 45 ms for 300 ms -> delta 25 */
	for (t = t0; t < t0 + 10000 * NS_MS; t += 10 * NS_MS)
		flow_add(&f, t, 20 * NS_MS);
	assert(flow_delta(&f, &C, t, &d) && fabs(d) < 1e-9);
	for (; t < t0 + 10300 * NS_MS; t += 10 * NS_MS)
		flow_add(&f, t, 45 * NS_MS);
	assert(flow_delta(&f, &C, t, &d) && fabs(d - 25) < 1e-9);
	/* baseline ages out after 30 s of 45 ms only */
	for (; t < t0 + 41000 * NS_MS; t += 10 * NS_MS)
		flow_add(&f, t, 45 * NS_MS);
	assert(flow_delta(&f, &C, t, &d) && fabs(d) < 1e-9);
	/* no qualification after silence > qual window */
	assert(!flow_delta(&f, &C, t + 600 * NS_MS, &d));
	/* only 2 samples in qual window -> not qualified */
	memset(&f, 0, sizeof(f));
	flow_add(&f, t0, 20 * NS_MS);
	flow_add(&f, t0 + 10 * NS_MS, 20 * NS_MS);
	assert(!flow_delta(&f, &C, t0 + 20 * NS_MS, &d));

	assert(median(v, 4) == 4 && median(v, 3) == 3);

	/* control law */
	assert(ctl_step(&C, &s, t0, false, 0, 0, false) == ACT_NOSIG && s.rate == 300);
	assert(ctl_step(&C, &s, t0, true, 12, 280, false) == ACT_DEC && fabs(s.rate - 266) < 1e-9);
	assert(ctl_step(&C, &s, t0 + 100 * NS_MS, true, 1, 266, false) == ACT_HOLDOFF && fabs(s.rate - 266) < 1e-9);
	assert(ctl_step(&C, &s, t0 + 300 * NS_MS, true, 1, 266, false) == ACT_INC && fabs(s.rate - 269.8) < 1e-9);
	assert(ctl_step(&C, &s, t0 + 400 * NS_MS, true, 1, 100, false) == ACT_HOLD);	/* under-utilised */
	assert(ctl_step(&C, &s, t0 + 500 * NS_MS, true, 5, 269, false) == ACT_HOLD);	/* between lo and hi */
	s.rate = 155;
	assert(ctl_step(&C, &s, t0, true, 50, 300, false) == ACT_DEC && s.rate == 150);	/* floor */
	s.rate = 379;
	s.holdoff_until = 0;
	assert(ctl_step(&C, &s, t0, true, 0, 379, false) == ACT_INC && s.rate == 380);	/* ceiling */

	/* v2: cut cooldown */
	s = (struct ctl_state){ .rate = 300 };
	assert(ctl_step(&C2, &s, t0, true, 20, 320, false) == ACT_DEC && fabs(s.rate - 285) < 1e-9);
	assert(fabs(s.target - 291) < 1e-9);					/* 300 * 0.97 */
	assert(ctl_step(&C2, &s, t0 + 100 * NS_MS, true, 20, 300, false) == ACT_COOLDOWN && fabs(s.rate - 285) < 1e-9);
	assert(ctl_step(&C2, &s, t0 + 399 * NS_MS, true, 20, 300, false) == ACT_COOLDOWN);
	assert(ctl_step(&C2, &s, t0 + 400 * NS_MS, true, 20, 300, false) == ACT_DEC);	/* cooldown over: cut again */
	assert(fabs(s.rate - 270.75) < 1e-9 && fabs(s.target - 276.45) < 1e-9);	/* new target from 285 */
	/* cur window longer than the cooldown also blocks */
	{
		struct ctl_cfg cw = C2;
		struct ctl_state s2 = { .rate = 300 };

		cw.cur_ns = 600 * NS_MS;
		ctl_step(&cw, &s2, t0, true, 20, 300, false);
		assert(ctl_step(&cw, &s2, t0 + 500 * NS_MS, true, 20, 300, false) == ACT_COOLDOWN);
		assert(ctl_step(&cw, &s2, t0 + 600 * NS_MS, true, 20, 300, false) == ACT_DEC);
	}
	/* v2: fast recovery to target, then slow probe */
	s = (struct ctl_state){ .rate = 300 };
	ctl_step(&C2, &s, t0, true, 20, 300, false);					/* 285, target 291 */
	assert(ctl_step(&C2, &s, t0 + 100 * NS_MS, true, 1, 285, false) == ACT_HOLDOFF);
	assert(ctl_step(&C2, &s, t0 + 300 * NS_MS, true, 1, 285, false) == ACT_INC_FAST);
	assert(fabs(s.rate - 291) < 1e-9 && s.target == 0);			/* 285 + 7.6 clamped to 291 */
	assert(ctl_step(&C2, &s, t0 + 400 * NS_MS, true, 1, 291, false) == ACT_INC && fabs(s.rate - 291.76) < 1e-9);
	/* v2: deep cut then several fast steps; a cut during the ramp resets the target */
	s = (struct ctl_state){ .rate = 300 };
	s.rate = 200;
	s.target = 291;
	assert(ctl_step(&C2, &s, t0, true, 1, 200, false) == ACT_INC_FAST && fabs(s.rate - 207.6) < 1e-9);
	assert(ctl_step(&C2, &s, t0 + 100 * NS_MS, true, 1, 207, false) == ACT_INC_FAST && fabs(s.rate - 215.2) < 1e-9);
	assert(ctl_step(&C2, &s, t0 + 200 * NS_MS, true, 15, 215, false) == ACT_DEC);
	assert(fabs(s.target - 215.2 * 0.97) < 1e-9 && fabs(s.rate - 215 * 0.95) < 1e-9);
	/* v2: signal between thresholds (single flow at 3-5 ms) now increases */
	s = (struct ctl_state){ .rate = 200 };
	assert(ctl_step(&C2, &s, t0, true, 4, 200, false) == ACT_INC);
	/* no signal: hold regardless of utilisation */
	assert(ctl_step(&C2, &s, t0, false, 0, 400, false) == ACT_NOSIG);


	/* v4: binding detector */
	{
		struct bind_hist b = { 0 };
		int i;

		for (i = 0; i < 4; i++)
			bind_push(&b, 5, true);
		assert(!bind_binding(&b, 0.8));			/* only 4 of 5 ticks seen yet */
		bind_push(&b, 5, false);
		assert(bind_binding(&b, 0.8));			/* 4/5 = 0.8 */
		bind_push(&b, 5, false);
		assert(!bind_binding(&b, 0.8));			/* 3/5 */
		for (i = 0; i < 5; i++)
			bind_push(&b, 5, true);
		assert(bind_binding(&b, 0.8));
	}
	/* v4: hardware quantum */
	assert(hw_quantum(100) == 1 && hw_quantum(127.9) == 1 && hw_quantum(128) == 10 && hw_quantum(380) == 10);
	assert(hw_quantum(1300) == 100);
	assert(hw_rate(159) == 150 && hw_rate(300) == 300 && hw_rate(99.6) == 99);
	{
		struct ctl_cfg C4 = C2;

		C4.bind_backlog_frac = 0.8;
		C4.probe_step = 10;
		C4.fast_step = 20;
		C4.probe_interval_ns = 500 * NS_MS;
		C4.ceil = 380;
		/* quantum probe while binding, then wait the probe interval */
		s = (struct ctl_state){ .rate = 300 };
		assert(ctl_step(&C4, &s, t0, true, 2, 300, true) == ACT_INC && s.rate == 310);
		assert(ctl_step(&C4, &s, t0 + 100 * NS_MS, true, 2, 310, true) == ACT_PROBEWAIT && s.rate == 310);
		assert(ctl_step(&C4, &s, t0 + 499 * NS_MS, true, 2, 310, true) == ACT_PROBEWAIT);
		assert(ctl_step(&C4, &s, t0 + 500 * NS_MS, true, 2, 310, true) == ACT_INC && s.rate == 320);
		/* signal in the old hold zone (6 ms) while binding: no increase (inc threshold 5 unchanged) */
		assert(ctl_step(&C4, &s, t0 + 2000 * NS_MS, true, 6, 320, true) == ACT_HOLD && s.rate == 320);
		/* no increase without backlog, whatever the signal or achieved */
		assert(ctl_step(&C4, &s, t0 + 3000 * NS_MS, true, 0.5, 400, false) == ACT_NOBIND && s.rate == 320);
		/* ramp-up must not lower the rate: achieved climbing from 0, no backlog yet, low signal */
		s = (struct ctl_state){ .rate = 300 };
		{
			int k;

			for (k = 0; k < 30; k++)
				ctl_step(&C4, &s, t0 + k * 100 * NS_MS, k > 3, 1, k * 10.0, false);
			assert(s.rate == 300);
			/* ... and also not with backlog but a 5-10 ms signal */
			for (k = 30; k < 40; k++)
				ctl_step(&C4, &s, t0 + k * 100 * NS_MS, true, 7, 290, true);
			assert(s.rate == 300);
		}
		/* cut: unchanged rule, result on the quantum; target on the quantum */
		s = (struct ctl_state){ .rate = 330 };
		assert(ctl_step(&C4, &s, t0, true, 15, 330, true) == ACT_DEC);
		assert(s.rate == 310 && s.target == 320);		/* 330*0.95 = 313.5 -> 310; 330*0.97 = 320.1 -> 320 */
		assert(ctl_step(&C4, &s, t0 + 100 * NS_MS, true, 15, 330, true) == ACT_COOLDOWN && s.rate == 310);
		/* fast recovery: +20 per tick up to the target, then quantum probes */
		assert(ctl_step(&C4, &s, t0 + 300 * NS_MS, true, 1, 310, true) == ACT_INC_FAST && s.rate == 320 && s.target == 0);
		assert(ctl_step(&C4, &s, t0 + 400 * NS_MS, true, 1, 320, true) == ACT_INC && s.rate == 330);
		/* deep cut: floor stays on the quantum and fast steps are 20 */
		s = (struct ctl_state){ .rate = 160 };
		assert(ctl_step(&C4, &s, t0, true, 30, 100, true) == ACT_DEC && s.rate == 150 && s.target == 150);
		s = (struct ctl_state){ .rate = 200, .target = 290 };
		assert(ctl_step(&C4, &s, t0, true, 1, 200, true) == ACT_INC_FAST && s.rate == 220);
		/* ceiling is only a bound */
		s = (struct ctl_state){ .rate = 380 };
		assert(ctl_step(&C4, &s, t0, true, 1, 380, true) == ACT_INC && s.rate == 380);
	}
	puts("ctl_test: all passed");
	return 0;
}
