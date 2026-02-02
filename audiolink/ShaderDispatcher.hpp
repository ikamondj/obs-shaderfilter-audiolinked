
#pragma once
#include <vector>
static inline const int FFT_SIZE = 2048;   // You can change later
class ShaderDispatcher {
public:
    static void Initialize(); // call once in Plugin.cpp
    static void Shutdown();   // remove timer
    static void Tick(void* data); // called by obs_timer_add
    static void ProcessSource(struct obs_source* src);
    static void ProcessShaderFilter(struct obs_source* filter);
    template <const int cap>
    static void processPcm(const std::array<float, cap>& buffer, struct AudioBands& g_audioDat, int writeIndex);
    static float hann[FFT_SIZE];
private:

};
