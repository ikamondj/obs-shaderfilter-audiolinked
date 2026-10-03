#include "shader-spectrum.h"
#include <pffft/pffft.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

using spectrum_ptr = std::unique_ptr<sf_spectrum, decltype(&sf_spectrum_destroy)>;
using bands = std::array<float, SF_SPECTRUM_BANDS>;
constexpr uint64_t now = 1000000000;

// Keep the existing instant-response checks focused on row zero of the texture.
static uint64_t read_fast_spectrum(sf_spectrum *fft, float *levels, uint64_t timestamp)
{
	std::array<float, SF_SPECTRUM_VALUES> texture{};
	const uint64_t revision = sf_spectrum_read(fft, texture.data(), timestamp);
	std::copy_n(texture.data(), SF_SPECTRUM_BANDS, levels);
	return revision;
}

static bands wait_result(sf_spectrum *fft, uint64_t previous)
{
	bands levels{};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while ((read_fast_spectrum(fft, levels.data(), now) / 2) <= (previous / 2)) {
		check(std::chrono::steady_clock::now() < deadline, "FFT worker did not produce a result");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (float level : levels)
		check(std::isfinite(level) && level >= 0 && level <= 1, "Spectrum escaped [0,1]");
	return levels;
}

static bands tone(sf_spectrum *fft, uint32_t rate, double frequency, float amplitude, bool antiphase = false)
{
	const size_t size = sf_spectrum_window_size(rate);
	std::vector<float> left(size), right(size);
	for (size_t i = 0; i < size; ++i) {
		left[i] = amplitude * float(std::sin(6.283185307179586 * frequency * double(i) / rate));
		right[i] = antiphase ? -left[i] : left[i];
	}
	const float *audio[] = {left.data(), right.data()};
	bands levels{};
	const uint64_t previous = read_fast_spectrum(fft, levels.data(), now);
	sf_spectrum_push(fft, audio, 2, uint32_t(size), rate, false, 0, now);
	return wait_result(fft, previous);
}

static double total_power(const bands &levels)
{
	double power = 0;
	for (float level : levels)
		if (level > 0)
			power += std::pow(10.0, (double(level) * 80.0 - 80.0) / 10.0);
	return power;
}

int main()
{
	check(std::abs(sf_spectrum_band_edge(0) - 20.0) < 1e-8, "Lowest edge mismatch");
	check(std::abs(sf_spectrum_band_edge(64) - 30000.0) < 1e-8, "Highest edge mismatch");
	const double ratio = std::pow(1500.0, 1.0 / 64.0);
	for (size_t band = 0; band < 64; ++band)
		check(std::abs(sf_spectrum_band_edge(band + 1) / sf_spectrum_band_edge(band) - ratio) < 1e-10,
		      "Bands are not equal-ratio");
	for (uint32_t rate : {44100u, 48000u, 96000u, 192000u}) {
		const size_t size = sf_spectrum_fft_size(rate);
		const size_t window = sf_spectrum_window_size(rate);
		check((size & (size - 1)) == 0 && size == 4 * window, "FFT padding mismatch");
		check(double(window) / rate >= 0.020 && double(window) / rate < 0.040,
		      "FFT retains more than one short live window");
	}
	check(std::abs(20 * std::log10(sf_spectrum_a_weight(1000))) < 0.01, "A weighting must be unity at 1 kHz");
	check(std::abs(20 * std::log10(sf_spectrum_a_weight(100)) + 19.14) < 0.05, "100 Hz A weighting mismatch");
	check(std::abs(20 * std::log10(sf_spectrum_a_weight(10000)) + 2.49) < 0.05, "10 kHz A weighting mismatch");
	check(!sf_spectrum_create(0, 2) && !sf_spectrum_create(48000, 0), "Invalid audio format accepted");
	spectrum_ptr fft(sf_spectrum_create(48000, 2), sf_spectrum_destroy);
	check(bool(fft), "Cannot allocate FFT worker");
	bands levels{};
	const auto silent_revision = read_fast_spectrum(fft.get(), levels.data(), now);
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	check(read_fast_spectrum(fft.get(), levels.data(), now) == silent_revision, "Idle worker performed an FFT");
	check(*std::max_element(levels.begin(), levels.end()) == 0, "Startup spectrum is not silent");

	// A single small callback must produce output without waiting for a full
	// window, a 30 Hz hop threshold, or a multi-second startup buffer.
	std::array<float, 256> attack{};
	for (size_t i = 0; i < attack.size(); ++i)
		attack[i] = 0.5f * float(std::sin(6.283185307179586 * 1000 * double(i) / 48000));
	const float *attack_audio[] = {attack.data(), attack.data()};
	sf_spectrum_push(fft.get(), attack_audio, 2, uint32_t(attack.size()), 48000, false, 0, now);
	const auto onset = wait_result(fft.get(), silent_revision);
	check(*std::max_element(onset.begin(), onset.end()) > 0.2f, "Small audio block did not trigger an immediate FFT");

	const auto normal = tone(fft.get(), 48000, 1000, 0.5f);
	const auto reversed = tone(fft.get(), 48000, 1000, 0.5f, true);
	for (size_t band = 0; band < 64; ++band)
		check(std::abs(normal[band] - reversed[band]) < 1e-6f, "Opposite stereo phase cancelled spectrum power");
	const auto peak = size_t(std::max_element(normal.begin(), normal.end()) - normal.begin());
	check(sf_spectrum_band_edge(peak) <= 1000 && sf_spectrum_band_edge(peak + 1) > 1000, "1 kHz mapped to wrong band");
	// A short window spreads a tone across adjacent regions; the sum of their
	// powers, rather than one peak texel, must retain the signal's mean square.
	check(std::abs(total_power(normal) - 0.125) < 0.002, "Window/FFT/channel normalization is incorrect");
	for (size_t band = 0; band < 64; ++band) {
		if (sf_spectrum_band_edge(band) >= 24000)
			check(normal[band] == 0, "Out-of-Nyquist bands contain invented energy");
	}
	// A tenfold amplitude drop must fully appear within one short window,
	// without attack/release smoothing, peak hold, or previous-frame averaging.
	const auto dropped = tone(fft.get(), 48000, 1000, 0.05f);
	check(std::abs(total_power(dropped) / total_power(normal) - 0.01) < 0.0001,
	      "Spectrum retained history after a sudden amplitude drop");
	// Reading a stopped source becomes silent without capture callbacks.
	read_fast_spectrum(fft.get(), levels.data(), now + 49000000);
	check(*std::max_element(levels.begin(), levels.end()) > 0, "Spectrum timed out before the stale cutoff");
	read_fast_spectrum(fft.get(), levels.data(), now + 51000000);
	check(*std::max_element(levels.begin(), levels.end()) == 0, "Stopped source retained stale spectrum");
	// Flush the live window using small callbacks, requiring a fresh result for
	// every one. At 48 kHz the entire decay must end within 1024 samples (21.3 ms).
	std::array<float, 128> silence{};
	const float *silent_audio[] = {silence.data(), silence.data()};
	const size_t live_window = sf_spectrum_window_size(48000);
	check(double(live_window) / 48000 < 0.022, "48 kHz spectrum window is too slow");
	for (size_t frame = 0; frame < live_window; frame += silence.size()) {
		const uint64_t previous = read_fast_spectrum(fft.get(), levels.data(), now);
		sf_spectrum_push(fft.get(), silent_audio, 2, uint32_t(silence.size()), 48000, false, 0, now);
		levels = wait_result(fft.get(), previous);
	}
	check(*std::max_element(levels.begin(), levels.end()) == 0, "Silence left a smoothed spectrum tail");

	(void)tone(fft.get(), 48000, 1000, 0.5f);
	sf_spectrum_push(fft.get(), nullptr, 2, 1024, 48000, true, 0, now);
	read_fast_spectrum(fft.get(), levels.data(), now);
	check(*std::max_element(levels.begin(), levels.end()) == 0, "Muted source retained spectrum");
	const auto muted_revision = read_fast_spectrum(fft.get(), levels.data(), now);
	std::this_thread::sleep_for(std::chrono::milliseconds(40));
	check(read_fast_spectrum(fft.get(), levels.data(), now) == muted_revision, "Muted worker performed an FFT");

	spectrum_ptr high_rate(sf_spectrum_create(96000, 2), sf_spectrum_destroy);
	check(bool(high_rate), "Cannot allocate 96 kHz FFT");
	const double high_frequency = std::sqrt(sf_spectrum_band_edge(63) * sf_spectrum_band_edge(64));
	const auto high = tone(high_rate.get(), 96000, high_frequency, 0.5f);
	check(std::max_element(high.begin(), high.end()) - high.begin() == 63 && high[63] > 0.5f,
	      "96 kHz input does not cover the highest requested band");
	// Silence must remain silent after a previous loud window.
	const auto quiet = tone(fft.get(), 48000, 1000, 0.0f);
	check(*std::max_element(quiet.begin(), quiet.end()) == 0, "Zero input produced spectrum energy");
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < 12; ++i)
		(void)tone(fft.get(), 48000, 1000, 0.5f);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 12;
	// Reconfigure the running worker, then prove the new Nyquist range is used.
	const uint64_t before_format = read_fast_spectrum(fft.get(), levels.data(), now) / 2;
	sf_spectrum_push(fft.get(), nullptr, 2, 1, 96000, true, 0, now);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (read_fast_spectrum(fft.get(), levels.data(), now) / 2 < before_format + 2) {
		check(std::chrono::steady_clock::now() < deadline, "Worker did not finish format reconfiguration");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto reconfigured = tone(fft.get(), 96000, high_frequency, 0.5f);
	check(reconfigured[63] > 0.5f, "Reconfigured worker retained the old sample rate");
	// Nonfinite samples are sanitized before they can poison FFT history.
	std::vector<float> invalid(sf_spectrum_window_size(96000), NAN);
	const float *bad_audio[] = {invalid.data(), nullptr};
	const uint64_t before_invalid = read_fast_spectrum(fft.get(), levels.data(), now);
	sf_spectrum_push(fft.get(), bad_audio, 2, uint32_t(invalid.size()), 96000, false, 0, now);
	const auto sanitized = wait_result(fft.get(), before_invalid);
	check(*std::max_element(sanitized.begin(), sanitized.end()) == 0, "Invalid samples poisoned spectrum history");
	// Destroy while a transform is pending/in flight; joining must be safe.
	std::fill(invalid.begin(), invalid.end(), 0.5f);
	sf_spectrum_push(fft.get(), bad_audio, 2, uint32_t(invalid.size()), 96000, false, 0, now);
	fft.reset();
	std::printf("Spectrum tests passed; PFFFT SIMD width %d; stereo 48 kHz job incl. test signal generation/wait: %.2f ms\n",
		    pffft_simd_size(), ms);
}
