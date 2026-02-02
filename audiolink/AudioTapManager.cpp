#include "AudioTapManager.hpp"


#include "AudioData.hpp"


AudioBands g_audioData;
AudioBands g_audioDataL;
AudioBands g_audioDataR;
std::array<float, AUDIO_BUFFER_CAPACITY> g_pcmBufferL;
std::array<float, AUDIO_BUFFER_CAPACITY> g_pcmBufferR;
std::atomic<int> g_pcmBufferWriteIndex = 0;
std::atomic<int> g_pcmBufferSize = 0;
std::atomic<bool> g_audioDataReady = false;
AudioTapManager g_manager;
std::atomic_bool& audioDataReady() {return g_audioDataReady;}
std::atomic_int& pcmBufferSize() {return g_pcmBufferSize;}
std::atomic_int& pcmWriteIndex() {return g_pcmBufferWriteIndex;}
AudioBands& leftAudioBands() {return g_audioDataL;}
AudioBands& rightAudioBands() {return g_audioDataR;}
AudioBands& combinedAudioBands() {return g_audioData;}
AudioTapManager* GetAudioTapManager() { return &g_manager; }
std::array<float, AUDIO_BUFFER_CAPACITY>& getLeftPcmBuffer() {return g_pcmBufferL;}
std::array<float, AUDIO_BUFFER_CAPACITY>& getRightPcmBuffer() {return g_pcmBufferR;}

// Mutex protects changing currentSource_
static std::mutex sourceMutex;

void AudioTapManager::SetActiveSource(const std::string& uuid)
{
    std::lock_guard<std::mutex> lock(sourceMutex);

    // Remove callback from previous source
    if (currentSource_) {
        obs_source_remove_audio_capture_callback(currentSource_, AudioCallback, this);
        obs_source_release(currentSource_);
        currentSource_ = nullptr;
    }

    // Blank selection / None
    if (uuid.empty())
        return;

    // Lookup new source
    obs_source_t* src = obs_get_source_by_uuid(uuid.c_str());
    if (!src) {
        blog(LOG_WARNING, "[AudioLink] Source with UUID %s not found", uuid.c_str());
        return;
    }

    // Retain reference so it stays alive
    obs_source_t* retained = obs_source_get_ref(src);

    // Register callback
    obs_source_add_audio_capture_callback(retained, AudioCallback, this);

    currentSource_ = retained;

    blog(LOG_INFO, "[AudioLink] Now tapping audio from source: %s", obs_source_get_name(retained));

    // Release lookup ref (but keep retained)
    obs_source_release(src);
}

void AudioTapManager::AudioCallback(void* param,
                                   obs_source* /*source*/,
                                   const struct audio_data* audio,
				bool muted)
{
    if (!audio || audio->frames == 0)
        return;

    // We assume float samples (OBS internal default)
    // Use channel 0 (left)
    const float* samplesL = reinterpret_cast<const float*>(audio->data[0]);
    const float* samplesR = audio->data[1] == NULL ? samplesL : reinterpret_cast<const float*>(audio->data[1]);

    const size_t frameCount = audio->frames;

    // Local copy to avoid reloading atomic inside tight loop
    int writeIndex = g_pcmBufferWriteIndex.load(std::memory_order_relaxed);

    // Write samples into ring buffer
    for (size_t i = 0; i < frameCount; i++) {
        g_pcmBufferL[writeIndex] = samplesL[i];
	g_pcmBufferR[writeIndex] = samplesR[i];
        writeIndex++;
        if (writeIndex >= AUDIO_BUFFER_CAPACITY)
            writeIndex = 0;
    }

    // Commit index once per block
    g_pcmBufferWriteIndex.store(writeIndex, std::memory_order_release);
}
