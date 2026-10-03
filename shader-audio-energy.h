#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Private audio analysis: full spectrum followed by bass/lows/mids/treb.
 * Store time-weighted linear power, before dB mapping or envelope release.
 * 300 ten-millisecond buckets bound memory; each capture block's mean power
 * is treated as constant over that block. The oldest bucket is prorated. */
#define SF_ENERGY_CHANNELS 5
#define SF_ENERGY_BUCKETS 300
#define SF_ENERGY_SECONDS 3.0
#define SF_ENERGY_STEP (SF_ENERGY_SECONDS / SF_ENERGY_BUCKETS)

struct sf_audio_energy {
	double history[SF_ENERGY_BUCKETS][SF_ENERGY_CHANNELS];
	double total[SF_ENERGY_CHANNELS];
	double pending[SF_ENERGY_CHANNELS];
	double pending_seconds;
	size_t next;
	uint64_t audio_end_ns;
	uint64_t capture_ns;
	double capture_seconds;
};

static inline void sf_audio_energy_push(struct sf_audio_energy *window, const double *power, double seconds)
{
	if (!isfinite(seconds) || seconds <= 0.0)
		return;
	if (seconds >= SF_ENERGY_SECONDS) {
		memset(window->history, 0, sizeof(window->history));
		memset(window->total, 0, sizeof(window->total));
		memset(window->pending, 0, sizeof(window->pending));
		window->next = 0;
		window->pending_seconds = 0.0;
		seconds = SF_ENERGY_SECONDS;
	}
	while (seconds > 1e-12) {
		const double step = fmin(seconds, SF_ENERGY_STEP - window->pending_seconds);
		for (size_t i = 0; i < SF_ENERGY_CHANNELS; ++i)
			window->pending[i] += (isfinite(power[i]) ? fmax(power[i], 0.0) : 0.0) * step;
		window->pending_seconds += step;
		seconds -= step;
		if (window->pending_seconds >= SF_ENERGY_STEP - 1e-12) {
			for (size_t i = 0; i < SF_ENERGY_CHANNELS; ++i) {
				window->total[i] += window->pending[i] - window->history[window->next][i];
				window->history[window->next][i] = window->pending[i];
				window->pending[i] = 0.0;
			}
			window->next = (window->next + 1) % SF_ENERGY_BUCKETS;
			window->pending_seconds = 0.0;
		}
	}
}

static inline void sf_audio_energy_capture(struct sf_audio_energy *window, const double *power, double seconds,
					   uint64_t timestamp_ns, uint64_t now_ns)
{
	const double silence[SF_ENERGY_CHANNELS] = {0};
	/* Discontinuous/backward source timestamps begin a new history. Forward
	 * gaps contribute silence, including when a stopped source resumes. */
	if (window->audio_end_ns && timestamp_ns) {
		if (timestamp_ns + 1000000 < window->audio_end_ns)
			memset(window, 0, sizeof(*window));
		else if (timestamp_ns > window->audio_end_ns)
			sf_audio_energy_push(window, silence, (double)(timestamp_ns - window->audio_end_ns) / 1e9);
	}
	sf_audio_energy_push(window, power, seconds);
	window->audio_end_ns = timestamp_ns ? timestamp_ns + (uint64_t)(seconds * 1e9) : 0;
	window->capture_ns = now_ns;
	window->capture_seconds = seconds;
}

/* Read without changing history, so multiple filters see the same result.
 * Missing callbacks age out as silence. Allow one capture block's duration
 * before doing so, avoiding decay between normally scheduled callbacks. */
static inline void sf_audio_energy_read(const struct sf_audio_energy *window, uint64_t now_ns, double *power)
{
	double silence = now_ns > window->capture_ns ? (double)(now_ns - window->capture_ns) / 1e9 : 0.0;
	silence = fmax(0.0, silence - window->capture_seconds);
	if (silence >= SF_ENERGY_SECONDS) {
		memset(power, 0, sizeof(double) * SF_ENERGY_CHANNELS);
		return;
	}
	for (size_t i = 0; i < SF_ENERGY_CHANNELS; ++i)
		power[i] = window->total[i] + window->pending[i];
	double remove = window->pending_seconds + silence;
	for (size_t bucket = 0; bucket < SF_ENERGY_BUCKETS && remove > 0.0; ++bucket) {
		const double fraction = fmin(remove / SF_ENERGY_STEP, 1.0);
		const size_t index = (window->next + bucket) % SF_ENERGY_BUCKETS;
		for (size_t i = 0; i < SF_ENERGY_CHANNELS; ++i)
			power[i] -= window->history[index][i] * fraction;
		remove -= SF_ENERGY_STEP;
	}
	if (remove > 0.0 && window->pending_seconds > 0.0) {
		for (size_t i = 0; i < SF_ENERGY_CHANNELS; ++i)
			power[i] -= window->pending[i] * fmin(remove / window->pending_seconds, 1.0);
	}
	for (size_t i = 0; i < SF_ENERGY_CHANNELS; ++i)
		power[i] = fmax(0.0, power[i] / SF_ENERGY_SECONDS);
}
