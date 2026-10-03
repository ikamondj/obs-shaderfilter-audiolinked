# Audio falloff helpers

`bass`, `lows`, `mids`, and `treb` are instantaneous block RMS levels in `[0,1]`,
with band-center A-weighting and a -60 to 0 dB mapping. They have no added
attack or release envelope.

```hlsl
float4 instant = linfoff(0.0); // Exactly float4(bass, lows, mids, treb)
float4 linear = linfoff(1.0);
float4 exponential = expfoff(1.0);
float4 per_band = expfoff(float4(0.0, 0.25, 0.5, 1.0));
```

Both helpers clamp their arguments to `[0,1]`. A scalar applies the same tail
to all bands; a `float4` selects separate tails in bass/lows/mids/treb order.

| Helper argument | Stored preset | Duration |
| --- | --- | --- |
| 0 | Base RMS | No extra decay |
| 0.25 | fast | 0.12 seconds |
| 0.5 | medium | 0.4 seconds |
| 0.75 | slow | 1.0 second |
| 1 | slug | 2.7307 seconds |

Intermediate arguments interpolate the neighboring stored values. There are
four scalar presets per band per family, not 64-element arrays or textures.
For example, `ef_bass_slug` and `lf_bass_slug` expose the longest bass presets.

For duration `T` and elapsed seconds `dt`:

- Linear: `max(previous - dt / T, current)`. Constant normalized-level slope;
  a full-scale value reaches zero after `T` seconds of silence.
- Exponential: `max(previous * exp(-log(100) * dt / T), current)`. Proportional
  decay; the value reaches 1% of its initial level after `T` seconds.

Rises are immediate. Decay uses audio time, not video frame count, and continues
when capture callbacks stop. The longest duration is comparable to the long
FFT window, although envelopes and windowed spectra respond differently.
There is no global falloff slider or release multiplier.

See [VARIABLE_REFERENCE.md](VARIABLE_REFERENCE.md#exponential-and-linear-falloff-variables)
for all preset names and details of weighting and timing.
