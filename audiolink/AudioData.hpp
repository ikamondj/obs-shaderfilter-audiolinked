#pragma once
#include <atomic>

struct AudioBands {
    float bass = 0.0f;
    float lows = 0.0f;
    float mids = 0.0f;
    float treb = 0.0f;

    float bass_c = 0.0f;
    float lows_c = 0.0f;
    float mids_c = 0.0f;
    float treb_c = 0.0f;

    float bins[64] = {};
};

extern AudioBands g_audioData;
extern std::atomic<bool> g_audioDataReady;
