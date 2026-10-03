#include "shader-spectrum.h"

#include <pffft/pffft.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr uint64_t stale_ns = 50000000;
constexpr size_t max_channels = 8;

struct aligned_deleter {
	void operator()(float *ptr) const { pffft_aligned_free(ptr); }
};
using aligned_buffer = std::unique_ptr<float, aligned_deleter>;

aligned_buffer allocate(size_t count)
{
	aligned_buffer buffer(static_cast<float *>(pffft_aligned_malloc(count * sizeof(float))));
	if (!buffer)
		throw std::bad_alloc();
	return buffer;
}

size_t slow_fft_size(uint32_t rate)
{
	// Restore the original resolution: four linear bins in the narrowest log
	// region. At 48 kHz this is 131072 samples, or about 2.73 seconds.
	const double required = double(rate) * 4.0 / (sf_spectrum_band_edge(1) - sf_spectrum_band_edge(0));
	size_t size = 32;
	while (double(size) < required)
		size *= 2;
	return size;
}

/* All FFT buffers, window coefficients and fractional bin weights are cached.
 * Each row analyzes its own window from one shared audio history. */
struct fft_analysis {
	struct bin_weight {
		size_t bin;
		double weight;
	};
	size_t size;
	size_t window_size;
	size_t channels;
	std::unique_ptr<PFFFT_Setup, decltype(&pffft_destroy_setup)> setup;
	aligned_buffer input;
	aligned_buffer output;
	aligned_buffer work;
	std::vector<float> samples;
	std::vector<float> window;
	std::vector<bin_weight> weights[SF_SPECTRUM_BANDS];
	double normalization;

	fft_analysis(uint32_t rate, size_t count, size_t history_size = 0)
		: size(history_size ? history_size : sf_spectrum_fft_size(rate)),
		  window_size(history_size ? history_size : sf_spectrum_window_size(rate)),
		  channels(count),
		  setup(pffft_new_setup(static_cast<int>(size), PFFFT_REAL), pffft_destroy_setup),
		  input(allocate(size)),
		  output(allocate(size)),
		  work(allocate(size)),
		  samples(window_size * count),
		  window(window_size)
	{
		if (!setup)
			throw std::bad_alloc();
		double window_power = 0.0;
		for (size_t i = 0; i < window_size; ++i) {
			window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * double(i) / double(window_size)));
			window_power += double(window[i]) * window[i];
		}
		// One-sided, window-energy-corrected mean square, averaged over channels.
		normalization = 2.0 / (double(size) * window_power * double(channels));
		const double bin_hz = double(rate) / double(size);
		for (size_t band = 0; band < SF_SPECTRUM_BANDS; ++band) {
			const double low = sf_spectrum_band_edge(band);
			const double high = std::min(sf_spectrum_band_edge(band + 1), rate * 0.5);
			if (low >= high)
				continue; // Preserve the 20–30000 Hz axis above Nyquist as zeros.
			const size_t first = std::max<size_t>(1, static_cast<size_t>(std::floor(low / bin_hz)));
			const size_t last = std::min(size / 2, static_cast<size_t>(std::ceil(high / bin_hz)));
			for (size_t bin = first; bin <= last; ++bin) {
				const double frequency = double(bin) * bin_hz;
				const double overlap =
					std::min(high, frequency + bin_hz * 0.5) - std::max(low, frequency - bin_hz * 0.5);
				if (overlap <= 0.0)
					continue;
				const double gain = sf_spectrum_a_weight(frequency);
				// Nyquist is real-only and owns a half-width frequency cell.
				weights[band].push_back({bin, overlap / bin_hz * gain * gain});
			}
		}
	}

	void transform(float *levels)
	{
		double power[SF_SPECTRUM_BANDS] = {};
		for (size_t channel = 0; channel < channels; ++channel) {
			const float *source = samples.data() + channel * window_size;
			for (size_t i = 0; i < window_size; ++i)
				input.get()[i] = source[i] * window[i];
			std::fill(input.get() + window_size, input.get() + size, 0.0f);
			pffft_transform_ordered(setup.get(), input.get(), output.get(), work.get(), PFFFT_FORWARD);
			for (size_t band = 0; band < SF_SPECTRUM_BANDS; ++band) {
				for (const auto &entry : weights[band]) {
					const double real = output.get()[entry.bin == size / 2 ? 1 : entry.bin * 2];
					const double imag = entry.bin == size / 2 ? 0.0 : output.get()[entry.bin * 2 + 1];
					power[band] += (real * real + imag * imag) * entry.weight;
				}
			}
		}
		for (size_t band = 0; band < SF_SPECTRUM_BANDS; ++band) {
			const double mean_square = power[band] * normalization;
			levels[band] =
				mean_square > 1e-8
					? static_cast<float>(std::clamp((10.0 * std::log10(mean_square) + 80.0) / 80.0, 0.0, 1.0))
					: 0.0f;
		}
	}
};
} // namespace

struct sf_spectrum {
	std::mutex mutex;
	std::condition_variable wake;
	std::thread worker;
	bool stop = false;
	bool silent = true;
	uint32_t rate;
	size_t channels;
	uint32_t requested_rate;
	size_t requested_channels;
	std::vector<float> ring;
	size_t position = 0;
	size_t pending_frames = 0;
	size_t slow_pending_frames = 0;
	uint64_t epoch = 0;
	uint64_t generation = 0;
	uint64_t audio_end_ns = 0;
	uint64_t capture_ns = 0;
	float levels[SF_SPECTRUM_VALUES] = {};
	std::unique_ptr<fft_analysis> fft;
	std::unique_ptr<fft_analysis> slow_fft;
	std::unique_ptr<fft_analysis> middle_fft[2];

	sf_spectrum(uint32_t sample_rate, size_t count)
		: rate(sample_rate),
		  channels(count),
		  requested_rate(sample_rate),
		  requested_channels(count),
		  ring(slow_fft_size(sample_rate) * count),
		  fft(std::make_unique<fft_analysis>(sample_rate, count)),
		  slow_fft(std::make_unique<fft_analysis>(sample_rate, count, slow_fft_size(sample_rate)))
	{
		// At 48 kHz: 8192 samples (171 ms), then 32768 samples (683 ms).
		for (size_t row = 0; row < 2; ++row)
			middle_fft[row] = std::make_unique<fft_analysis>(sample_rate, count, slow_fft->size >> (4 - 2 * row));
		worker = std::thread(&sf_spectrum::run, this);
	}

	~sf_spectrum()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = true;
		}
		wake.notify_one();
		worker.join();
	}

	void clear()
	{
		silent = true;
		std::fill(ring.begin(), ring.end(), 0.0f);
		std::fill(std::begin(levels), std::end(levels), 0.0f);
		position = pending_frames = slow_pending_frames = 0;
		audio_end_ns = 0;
		++epoch; // Discard an in-flight transform after mute/seek/format change.
		++generation;
	}

	void copy_window(fft_analysis &analysis)
	{
		const size_t ring_size = slow_fft->window_size;
		const size_t size = analysis.window_size;
		const size_t start = (position + ring_size - size) & (ring_size - 1);
		const size_t first = std::min(size, ring_size - start);
		for (size_t channel = 0; channel < channels; ++channel) {
			float *dest = analysis.samples.data() + channel * size;
			const float *source = ring.data() + channel * ring_size;
			std::memcpy(dest, source + start, first * sizeof(float));
			std::memcpy(dest + first, source, (size - first) * sizeof(float));
		}
	}

	void run()
	{
		std::unique_lock<std::mutex> lock(mutex);
		while (true) {
			wake.wait(lock, [&] {
				return stop || requested_rate != rate || requested_channels != channels || pending_frames != 0;
			});
			if (stop)
				return;
			if (requested_rate != rate || requested_channels != channels) {
				const uint32_t next_rate = requested_rate;
				const size_t next_channels = requested_channels;
				lock.unlock();
				std::unique_ptr<fft_analysis> next_fft;
				std::unique_ptr<fft_analysis> next_slow_fft;
				std::unique_ptr<fft_analysis> next_middle_fft[2];
				std::vector<float> next_ring;
				try {
					next_fft = std::make_unique<fft_analysis>(next_rate, next_channels);
					next_slow_fft =
						std::make_unique<fft_analysis>(next_rate, next_channels, slow_fft_size(next_rate));
					for (size_t row = 0; row < 2; ++row)
						next_middle_fft[row] = std::make_unique<fft_analysis>(
							next_rate, next_channels, next_slow_fft->size >> (4 - 2 * row));
					next_ring.resize(next_slow_fft->window_size * next_channels);
				} catch (...) {
					// Stay silent after allocation failure until another format or
					// subscription change; never allocate on the capture thread.
					next_fft.reset();
					next_slow_fft.reset();
					for (auto &middle : next_middle_fft)
						middle.reset();
					next_ring.clear();
				}
				lock.lock();
				fft = std::move(next_fft);
				slow_fft = std::move(next_slow_fft);
				for (size_t row = 0; row < 2; ++row)
					middle_fft[row] = std::move(next_middle_fft[row]);
				ring = std::move(next_ring);
				rate = next_rate;
				channels = next_channels;
				clear();
				continue;
			}
			if (!fft) {
				pending_frames = 0;
				continue;
			}
			copy_window(*fft);
			// Keep the three longer transforms at the original ~30 Hz cadence.
			// The short transform still runs for each new callback snapshot.
			const bool update_slow = slow_pending_frames >= rate / 30;
			if (update_slow) {
				for (auto &middle : middle_fft)
					copy_window(*middle);
				copy_window(*slow_fft);
				slow_pending_frames = 0;
			}
			pending_frames = 0;
			const uint64_t captured_epoch = epoch;
			lock.unlock();
			float result[SF_SPECTRUM_VALUES];
			fft->transform(result);
			if (update_slow) {
				for (size_t row = 0; row < 2; ++row)
					middle_fft[row]->transform(result + (row + 1) * SF_SPECTRUM_BANDS);
				slow_fft->transform(result + (SF_SPECTRUM_ROWS - 1) * SF_SPECTRUM_BANDS);
			}
			lock.lock();
			if (captured_epoch == epoch) {
				std::copy_n(result, update_slow ? SF_SPECTRUM_VALUES : SF_SPECTRUM_BANDS, levels);
				++generation;
			}
		}
	}
};

size_t sf_spectrum_window_size(uint32_t sample_rate)
{
	// Prioritize live response: about 20 ms, rounded up to a power of two.
	const double required = double(sample_rate) * 0.020;
	size_t size = 32;
	while (double(size) < required)
		size *= 2;
	return size;
}

size_t sf_spectrum_fft_size(uint32_t sample_rate)
{
	return sf_spectrum_window_size(sample_rate) * 4;
}

double sf_spectrum_band_edge(size_t edge)
{
	return SF_SPECTRUM_MIN_HZ * std::pow(SF_SPECTRUM_MAX_HZ / SF_SPECTRUM_MIN_HZ, double(edge) / SF_SPECTRUM_BANDS);
}

double sf_spectrum_a_weight(double frequency)
{
	// Analog A-weighting magnitude, normalized at 1 kHz (approximately +2 dB).
	// Apply to FFT power before pooling, not to already logarithmic levels.
	const double f2 = frequency * frequency;
	return std::pow(10.0, 2.0 / 20.0) * (12194.0 * 12194.0 * f2 * f2) /
	       ((f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0));
}

sf_spectrum *sf_spectrum_create(uint32_t sample_rate, size_t channels)
{
	if (sample_rate < 8000 || sample_rate > 384000 || !channels || channels > max_channels)
		return nullptr;
	try {
		return new sf_spectrum(sample_rate, channels);
	} catch (...) {
		return nullptr;
	}
}

void sf_spectrum_destroy(sf_spectrum *spectrum)
{
	delete spectrum;
}

void sf_spectrum_push(sf_spectrum *spectrum, const float *const *audio, size_t channels, uint32_t frames, uint32_t sample_rate,
		      bool muted, uint64_t timestamp_ns, uint64_t now_ns)
{
	if (!spectrum || !frames || sample_rate < 8000 || sample_rate > 384000 || !channels || channels > max_channels)
		return;
	std::lock_guard<std::mutex> lock(spectrum->mutex);
	const bool capture_gap = spectrum->capture_ns && now_ns > spectrum->capture_ns && now_ns - spectrum->capture_ns > stale_ns;
	spectrum->capture_ns = now_ns;
	if (spectrum->requested_rate != sample_rate || spectrum->requested_channels != channels) {
		spectrum->requested_rate = sample_rate;
		spectrum->requested_channels = channels;
		spectrum->clear();
		spectrum->wake.notify_one();
	}
	if (spectrum->rate != sample_rate || spectrum->channels != channels || !spectrum->fft)
		return;
	if (muted || !audio) {
		if (!spectrum->silent)
			spectrum->clear();
		return;
	}
	if (capture_gap)
		spectrum->clear();
	if (timestamp_ns && spectrum->audio_end_ns) {
		const uint64_t diff = timestamp_ns > spectrum->audio_end_ns ? timestamp_ns - spectrum->audio_end_ns
									    : spectrum->audio_end_ns - timestamp_ns;
		if (diff > 100000000)
			spectrum->clear();
	}
	spectrum->audio_end_ns = timestamp_ns ? timestamp_ns + uint64_t(double(frames) * 1e9 / sample_rate) : 0;
	spectrum->silent = false;
	const size_t size = spectrum->slow_fft->window_size;
	// Only the newest window matters, even if a source supplies a large block.
	const size_t skip = frames > size ? frames - size : 0;
	const size_t count = frames - skip;
	for (size_t channel = 0; channel < channels; ++channel) {
		float *dest = spectrum->ring.data() + channel * size;
		const float *source = audio[channel];
		for (size_t i = 0; i < count; ++i) {
			const float sample = source ? source[skip + i] : 0.0f;
			dest[(spectrum->position + i) & (size - 1)] = std::isfinite(sample) ? sample : 0.0f;
		}
	}
	spectrum->position = (spectrum->position + count) & (size - 1);
	// All rows share one history buffer. Coalesce callbacks into the newest
	// snapshot; only the three longer windows wait for their hop threshold.
	spectrum->pending_frames = count;
	spectrum->slow_pending_frames = std::min(size, spectrum->slow_pending_frames + count);
	spectrum->wake.notify_one();
}

uint64_t sf_spectrum_read(sf_spectrum *spectrum, float *levels, uint64_t now_ns)
{
	if (!spectrum) {
		std::fill_n(levels, SF_SPECTRUM_VALUES, 0.0f);
		return 0;
	}
	std::lock_guard<std::mutex> lock(spectrum->mutex);
	const bool stale = now_ns > spectrum->capture_ns && now_ns - spectrum->capture_ns > stale_ns;
	if (stale)
		std::fill_n(levels, SF_SPECTRUM_VALUES, 0.0f);
	else
		std::copy_n(spectrum->levels, SF_SPECTRUM_VALUES, levels);
	return spectrum->generation * 2 + uint64_t(stale);
}
