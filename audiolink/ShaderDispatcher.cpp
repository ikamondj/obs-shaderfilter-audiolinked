
#include "ShaderDispatcher.hpp"
#include <obs.h>
#include <obs-source.h>
#include <obs-frontend-api.h>
#include <cstring>
#include "kissfft/kiss_fftr.h"
#include "AudioData.hpp"
#include "AudioTapManager.hpp"
#include <cmath>


static float fftInput[FFT_SIZE];
static kiss_fft_cpx fftOutput[FFT_SIZE / 2 + 1];
static kiss_fftr_cfg fftCfg = nullptr;

static void OnTick(void *data, float seconds)
{
    ShaderDispatcher::Tick(data);
}


static inline bool is_shader_filter(const obs_source_t* filter)
{
    if (!filter)
        return false;

    const char* id = obs_source_get_id(filter);
    return id && strcmp(id, "shader_filter") == 0;
}

void init_hann() {
    for (int i = 0; i < FFT_SIZE; i++) {
        ShaderDispatcher::hann[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FFT_SIZE - 1)));
    }
}


void ShaderDispatcher::Initialize()
{
	if (!fftCfg) {
		fftCfg = kiss_fftr_alloc(FFT_SIZE, 0, nullptr, nullptr);
	}

    	obs_add_tick_callback(OnTick, nullptr);
}

void ShaderDispatcher::Shutdown()
{
    	obs_remove_tick_callback(OnTick, nullptr);
	if (fftCfg) {
		free(fftCfg);
		fftCfg = nullptr;
	}

}

template <const int cap>
float mean_mag(int start, int end, std::array<float, cap>& mags){
    float s = 0.f;
    int n = 0;
    for (int i = start; i < end; ++i) { s += mags[i]; ++n; }
    return n ? s / n : 0.f;
};
inline float fracf(float x) { return x - floorf(x); }
template <const int cap>
void ShaderDispatcher::processPcm(const std::array<float, cap>& buffer, AudioBands& g_audioDat, int writeIndex)
{
    // Read last FFT_SIZE samples, wrapping ring buffer if needed
    for (int i = 0; i < FFT_SIZE; i++) {
        int idx = writeIndex - FFT_SIZE + i;
        if (idx < 0)
            idx += AUDIO_BUFFER_CAPACITY;
        fftInput[i] = buffer[idx] * hann[i];
    }

    // Run FFT
    kiss_fftr(fftCfg, fftInput, fftOutput);

    // Convert to magnitudes
    std::array<float, FFT_SIZE / 2> mags;
    for (int i = 0; i < FFT_SIZE / 2; i++) {
        float real = fftOutput[i].r;
        float imag = fftOutput[i].i;
        mags[i] = sqrtf(real * real + imag * imag);
    }

    // === Bass perceptual scaling ===
    // The ear is less sensitive to low frequencies; we reduce their energy a bit.
    // Also compensate for FFT’s 1/f power trend (bass dominates higher bins).
    const float sampleRate = 48000.0f;
    for (int i = 1; i < FFT_SIZE / 2; i++) {
        float freq = (sampleRate * i) / FFT_SIZE;
        // Approximate equal-loudness correction (downweights bass)
        float loudnessWeight = 1.0f;
        if (freq < 100.0f) {
            // Strong attenuation for sub-bass
            loudnessWeight = 0.25f + 0.75f * (freq / 100.0f);
        } else if (freq < 1000.0f) {
            // Gradual ramp up to flat around 1kHz
            loudnessWeight = 0.7f + 0.3f * (freq / 1000.0f);
        } else {
            // Slightly de-emphasize treble to avoid hiss dominance
            loudnessWeight = 1.0f - 0.15f * std::min((freq - 1000.0f) / 8000.0f, 1.0f);
        }

        // Correct for 1/f energy density bias (bass bins are denser in log scale)
        float spectralComp = sqrtf(freq / 1000.0f) * .041f;
        mags[i] *= loudnessWeight * spectralComp;
    }

    // Compute frequency bands (no normalization)
    int N = FFT_SIZE / 2;
    int q = N / 4;
    g_audioDat.bass = mean_mag(0, q, mags);
    g_audioDat.lows = mean_mag(q, 2 * q, mags);
    g_audioDat.mids = mean_mag(2 * q, 3 * q, mags);
    g_audioDat.treb = mean_mag(3 * q, N, mags);

    // "C" versions can be smoothed later:
    g_audioDat.bass_c = fracf(g_audioDat.bass_c + g_audioDat.bass * 0.002666667f);
    g_audioDat.lows_c = fracf(g_audioDat.lows_c + g_audioDat.lows * 0.002666667f);
    g_audioDat.mids_c = fracf(g_audioDat.mids_c + g_audioDat.mids * 0.002666667f);
    g_audioDat.treb_c = fracf(g_audioDat.treb_c + g_audioDat.treb * 0.002666667f);

    // Fill 64-bin visualization table (still unnormalized)
    const int bins = 64;
    for (int i = 0; i < bins; i++) {
        int srcIndex = (i * (FFT_SIZE / 2)) / bins;
        g_audioData.bins[i] = mags[srcIndex];
    }
}

void ShaderDispatcher::Tick(void*)
{
	// Snapshot write index
    int writeIndex = pcmWriteIndex().load(std::memory_order_acquire);

    auto& l = leftAudioBands();
    auto& r = rightAudioBands();
    processPcm(getLeftPcmBuffer(), l, writeIndex);
    processPcm(getRightPcmBuffer(), r, writeIndex);

    auto& c = combinedAudioBands();
    c.bass = (l.bass + r.bass) * 0.5f;
    c.lows = (l.lows + r.lows) * 0.5f;
    c.mids = (l.mids + r.mids) * 0.5f;
    c.treb = (l.treb + r.treb) * 0.5f;
    c.bass_c = fracf(c.bass_c + c.bass * 0.002666667f);
    c.lows_c = fracf(c.lows_c + c.lows * 0.002666667f);
    c.mids_c = fracf(c.mids_c + c.mids * 0.002666667f);
    c.treb_c = fracf(c.treb_c + c.treb * 0.002666667f);
    const int bins = 64;
    for (int i = 0; i < bins; i++) {
        int srcIndex = (i * (FFT_SIZE/2)) / bins;
        c.bins[i] = (l.bins[i]+r.bins[i]) * 0.5f;
    }

    g_audioDataReady.store(true, std::memory_order_release);
    // Enumerate all top-level sources in scenes
    obs_enum_sources([](void*, obs_source_t* src){
        ProcessSource(src);
        return true;
    }, nullptr);
}

static void EnumFilterCallback(obs_source_t* parent, obs_source_t* child, void* param)
{
    (void)parent;
    if (!child) return;

    // Process only shader filters (id == "shader_filter")
    const char* id = obs_source_get_id(child);
    if (id && strcmp(id, "shader_filter") == 0) {
        ShaderDispatcher::ProcessShaderFilter(child);
    }

    // Recurse: filters can have their own filters
    obs_source_enum_filters(child, EnumFilterCallback, param);
}

// Enumerate filters on a source:
void ShaderDispatcher::ProcessSource(obs_source_t* src)
{
    if (!src) return;
    obs_source_enum_filters(src, EnumFilterCallback, nullptr);
}

static void set_bins_array(obs_data_t *settings, const char *name, float*bins) {
    obs_data_array_t *arr = obs_data_array_create();
    for (int i = 0; i < 64; i++) {
        obs_data_t *obj = obs_data_create();
        obs_data_set_double(obj, "v", (double)bins[i]);
        obs_data_array_push_back(arr, obj);
        obs_data_release(obj);
    }
    obs_data_set_array(settings, name, arr);
    obs_data_array_release(arr);
}

void ShaderDispatcher::ProcessShaderFilter(obs_source_t* filter)
{
	if (!is_shader_filter(filter))
		return;
	if (!g_audioDataReady.load(std::memory_order_acquire))
		return;


	AudioBands& c = combinedAudioBands();
	AudioBands& l = leftAudioBands();
	AudioBands& r = rightAudioBands();

	obs_data_t *settings = obs_source_get_settings(filter);

	obs_data_set_double(settings, "bass",   c.bass);
	obs_data_set_double(settings, "lows",   c.lows);
	obs_data_set_double(settings, "mids",   c.mids);
	obs_data_set_double(settings, "treb",   c.treb);
	obs_data_set_double(settings, "bass_c", c.bass_c);
	obs_data_set_double(settings, "lows_c", c.lows_c);
	obs_data_set_double(settings, "mids_c", c.mids_c);
	obs_data_set_double(settings, "treb_c", c.treb_c);

	obs_data_set_double(settings, "l_bass",   l.bass);
	obs_data_set_double(settings, "l_lows",   l.lows);
	obs_data_set_double(settings, "l_mids",   l.mids);
	obs_data_set_double(settings, "l_treb",   l.treb);
	obs_data_set_double(settings, "l_bass_c", l.bass_c);
	obs_data_set_double(settings, "l_lows_c", l.lows_c);
	obs_data_set_double(settings, "l_mids_c", l.mids_c);
	obs_data_set_double(settings, "l_treb_c", l.treb_c);

	obs_data_set_double(settings, "r_bass",   r.bass);
	obs_data_set_double(settings, "r_lows",   r.lows);
	obs_data_set_double(settings, "r_mids",   r.mids);
	obs_data_set_double(settings, "r_treb",   r.treb);
	obs_data_set_double(settings, "r_bass_c", r.bass_c);
	obs_data_set_double(settings, "r_lows_c", r.lows_c);
	obs_data_set_double(settings, "r_mids_c", r.mids_c);
	obs_data_set_double(settings, "r_treb_c", r.treb_c);


// There was no bin-array support originally
	obs_source_update(filter, settings);
	obs_data_release(settings);
}
