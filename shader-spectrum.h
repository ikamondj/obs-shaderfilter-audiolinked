#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SF_SPECTRUM_BANDS 64
#define SF_SPECTRUM_ROWS 4
#define SF_SPECTRUM_VALUES (SF_SPECTRUM_BANDS * SF_SPECTRUM_ROWS)
#define SF_SPECTRUM_MIN_HZ 20.0
#define SF_SPECTRUM_MAX_HZ 30000.0

#ifdef __cplusplus
extern "C" {
#endif

/* Private shared-source FFT worker. Creation/destruction belong to the control
 * thread. Push copies audio only; transforms run on the worker, never while
 * holding the input/snapshot mutex. Channels are analyzed independently. */
struct sf_spectrum;
struct sf_spectrum *sf_spectrum_create(uint32_t sample_rate, size_t channels);
void sf_spectrum_destroy(struct sf_spectrum *spectrum);
void sf_spectrum_push(struct sf_spectrum *spectrum, const float *const *audio, size_t channels, uint32_t frames,
		      uint32_t sample_rate, bool muted, uint64_t timestamp_ns, uint64_t now_ns);
/* Copies SF_SPECTRUM_VALUES normalized levels in row-major order: instant,
 * short intermediate, long intermediate, long window. The returned revision changes on new FFT
 * output or timeout-to-silence, allowing the renderer to skip redundant uploads. */
uint64_t sf_spectrum_read(struct sf_spectrum *spectrum, float *levels, uint64_t now_ns);
/* Short live window (20–40 ms depending on rate), zero-padded for denser
 * frequency sampling. Padding does not improve actual frequency resolution. */
size_t sf_spectrum_window_size(uint32_t sample_rate);
size_t sf_spectrum_fft_size(uint32_t sample_rate);
double sf_spectrum_band_edge(size_t edge);
double sf_spectrum_a_weight(double frequency);

#ifdef __cplusplus
}
#endif
