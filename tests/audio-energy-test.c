#include "shader-audio-energy.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
			exit(1); \
		} \
	} while (0)

static void expect(const struct sf_audio_energy *window, uint64_t now, const double *expected, double scale)
{
	double power[SF_ENERGY_CHANNELS];
	sf_audio_energy_read(window, now, power);
	for (size_t i = 0; i < SF_ENERGY_CHANNELS; ++i)
		CHECK(fabs(power[i] - expected[i] * scale) < 1e-10);
}

int main(void)
{
	const double power[SF_ENERGY_CHANNELS] = {1.0, 0.01, 0.04, 0.09, 0.16};
	const double silence[SF_ENERGY_CHANNELS] = {0};
	struct sf_audio_energy window = {0};
	expect(&window, 0, silence, 1.0);
	sf_audio_energy_push(&window, power, 1.5);
	expect(&window, 0, power, 0.5); // Startup includes silence before capture began.
	sf_audio_energy_push(&window, power, 1.5);
	expect(&window, 0, power, 1.0);
	sf_audio_energy_push(&window, power, 100.0);
	expect(&window, 0, power, 1.0);
	sf_audio_energy_push(&window, silence, 1.501);
	expect(&window, 0, power, 1.499 / 3.0); // Fractional oldest bucket.
	sf_audio_energy_push(&window, silence, 1.498);
	expect(&window, 0, power, 0.001 / 3.0);
	sf_audio_energy_push(&window, silence, 0.001);
	expect(&window, 0, silence, 1.0);

	// Variable callback sizes and sample rates must measure seconds, not calls.
	const unsigned rates[] = {44100, 48000, 96000};
	for (size_t rate = 0; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
		memset(&window, 0, sizeof(window));
		unsigned frames = rates[rate] * 3;
		while (frames) {
			const unsigned block = frames > 1024 ? 1024 : frames;
			sf_audio_energy_push(&window, power, (double)block / rates[rate]);
			frames -= block;
		}
		expect(&window, 0, power, 1.0);
	}

	// Weight by time: a 0.1 s loud block then 2.9 s silence is 1/30 power.
	memset(&window, 0, sizeof(window));
	sf_audio_energy_push(&window, power, 0.1);
	sf_audio_energy_push(&window, silence, 2.9);
	expect(&window, 0, power, 1.0 / 30.0);
	sf_audio_energy_push(&window, silence, 0.1);
	expect(&window, 0, silence, 1.0);

	// A stopped source fades without callbacks; readers never alter history.
	memset(&window, 0, sizeof(window));
	sf_audio_energy_capture(&window, power, 3.0, 1000000000, 1000000000);
	expect(&window, 1000000000, power, 1.0);
	expect(&window, 5500000000, power, 0.5);
	expect(&window, 7000000000, silence, 1.0);
	expect(&window, 1000000000, power, 1.0);
	// Resume after a three-second gap; old energy has expired.
	sf_audio_energy_capture(&window, power, 0.03, 7000000000, 7000000000);
	expect(&window, 7000000000, power, 0.01);
	// A backward timestamp resets history rather than blending two timelines.
	sf_audio_energy_capture(&window, power, 0.06, 1000000000, 8000000000);
	expect(&window, 8000000000, power, 0.02);

	// Malformed time/power cannot poison later valid audio.
	memset(&window, 0, sizeof(window));
	const double invalid[SF_ENERGY_CHANNELS] = {NAN, INFINITY, -1.0, -INFINITY, NAN};
	sf_audio_energy_push(&window, power, NAN);
	sf_audio_energy_push(&window, power, -1.0);
	sf_audio_energy_push(&window, invalid, 3.0);
	expect(&window, 0, silence, 1.0);
	sf_audio_energy_push(&window, power, 3.0);
	expect(&window, 0, power, 1.0);
	puts("Audio energy window tests passed");
	return 0;
}
