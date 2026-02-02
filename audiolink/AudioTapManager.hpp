#pragma once
#include <string>
#include <obs.h>
#include <array>
#include <mutex>

class AudioTapManager {
public:
    void SetActiveSource(const std::string& uuid);

private:
    static void AudioCallback(void* param, obs_source* source, const struct audio_data* audio, bool muted);

    obs_source_t* currentSource_ = nullptr;
};

// global access
AudioTapManager* GetAudioTapManager();

inline const int AUDIO_BUFFER_CAPACITY = 16384;
std::array<float, AUDIO_BUFFER_CAPACITY>& getLeftPcmBuffer();
std::array<float, AUDIO_BUFFER_CAPACITY>& getRightPcmBuffer();
std::atomic_bool& audioDataReady();
std::atomic_int& pcmBufferSize();
std::atomic_int& pcmWriteIndex();
struct AudioBands& leftAudioBands();
struct AudioBands& rightAudioBands();
struct AudioBands& combinedAudioBands();
