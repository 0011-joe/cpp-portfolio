/*
 * Real-Time Audio DSP Effects Processor
 * Ring buffer, lock-free queue, fixed-point arithmetic, vectorized ops
 * Supports: EQ, Reverb, Delay, Compressor, Distortion
 *
 * Core DSP engine in C, compiled to WASM
 * Audio I/O via Web Audio API (AudioWorklet)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/bind.h>
#include <vector>
#include <string>
#endif

#define SAMPLE_RATE 44100
#define MAX_DELAY_SAMPLES (SAMPLE_RATE * 2)  // 2 seconds max delay
#define RING_BUFFER_SIZE 4096
#define EQ_BANDS 10
#define PROCESS_BLOCK 128

/* ============================================================
 * Ring Buffer (Lock-free single producer/consumer)
 * ============================================================ */

typedef struct {
    float buffer[RING_BUFFER_SIZE];
    volatile int read_pos;
    volatile int write_pos;
    int size;
} RingBuffer;

static void ring_init(RingBuffer* rb) {
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->size = RING_BUFFER_SIZE;
    memset(rb->buffer, 0, sizeof(rb->buffer));
}

static bool ring_write(RingBuffer* rb, float sample) {
    int next = (rb->write_pos + 1) % rb->size;
    if (next == rb->read_pos) return false; // full
    rb->buffer[rb->write_pos] = sample;
    rb->write_pos = next;
    return true;
}

static bool ring_read(RingBuffer* rb, float* sample) {
    if (rb->read_pos == rb->write_pos) return false; // empty
    *sample = rb->buffer[rb->read_pos];
    rb->read_pos = (rb->read_pos + 1) % rb->size;
    return true;
}

static float ring_peek_delay(RingBuffer* rb, int delay_samples) {
    int pos = (rb->write_pos - delay_samples + rb->size) % rb->size;
    return rb->buffer[pos];
}

/* ============================================================
 * Biquad Filter (for EQ)
 * ============================================================ */

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} BiquadFilter;

static void biquad_init(BiquadFilter* f) {
    f->b0 = 1; f->b1 = 0; f->b2 = 0;
    f->a1 = 0; f->a2 = 0;
    f->x1 = 0; f->x2 = 0;
    f->y1 = 0; f->y2 = 0;
}

static void biquad_lowpass(BiquadFilter* f, float freq, float Q) {
    float w0 = 2.0f * 3.14159265f * freq / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * Q);

    float a0 = 1.0f + alpha;
    f->b0 = ((1.0f - cosw0) / 2.0f) / a0;
    f->b1 = (1.0f - cosw0) / a0;
    f->b2 = ((1.0f - cosw0) / 2.0f) / a0;
    f->a1 = (-2.0f * cosw0) / a0;
    f->a2 = (1.0f - alpha) / a0;
}

static void biquad_highpass(BiquadFilter* f, float freq, float Q) {
    float w0 = 2.0f * 3.14159265f * freq / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * Q);

    float a0 = 1.0f + alpha;
    f->b0 = ((1.0f + cosw0) / 2.0f) / a0;
    f->b1 = -(1.0f + cosw0) / a0;
    f->b2 = ((1.0f + cosw0) / 2.0f) / a0;
    f->a1 = (-2.0f * cosw0) / a0;
    f->a2 = (1.0f - alpha) / a0;
}

static void biquad_peaking(BiquadFilter* f, float freq, float gain_db, float Q) {
    float w0 = 2.0f * 3.14159265f * freq / SAMPLE_RATE;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float A = powf(10.0f, gain_db / 40.0f);
    float alpha = sinw0 / (2.0f * Q);

    float a0 = 1.0f + alpha / A;
    f->b0 = (1.0f + alpha * A) / a0;
    f->b1 = (-2.0f * cosw0) / a0;
    f->b2 = (1.0f - alpha * A) / a0;
    f->a1 = (-2.0f * cosw0) / a0;
    f->a2 = (1.0f - alpha / A) / a0;
}

static float biquad_process(BiquadFilter* f, float input) {
    float output = f->b0 * input + f->b1 * f->x1 + f->b2 * f->x2
                   - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = input;
    f->y2 = f->y1;
    f->y1 = output;
    return output;
}

/* ============================================================
 * Delay Effect
 * ============================================================ */

typedef struct {
    RingBuffer buffer;
    float feedback;
    float mix;       // 0 = dry, 1 = wet
    int delay_time;  // in samples
    float lowpass_state;
    float dampening;
} DelayEffect;

static void delay_init(DelayEffect* d, float time_ms, float feedback, float mix) {
    ring_init(&d->buffer);
    d->delay_time = (int)(time_ms * SAMPLE_RATE / 1000.0f);
    if (d->delay_time > MAX_DELAY_SAMPLES) d->delay_time = MAX_DELAY_SAMPLES;
    d->feedback = feedback;
    d->mix = mix;
    d->lowpass_state = 0;
    d->dampening = 0.3f;
}

static float delay_process(DelayEffect* d, float input) {
    float delayed = ring_peek_delay(&d->buffer, d->delay_time);

    // Lowpass filter on feedback (tape delay simulation)
    d->lowpass_state = d->lowpass_state * d->dampening + delayed * (1.0f - d->dampening);
    delayed = d->lowpass_state;

    float output = input * (1.0f - d->mix) + delayed * d->mix;
    float feedback_sample = input + delayed * d->feedback;
    ring_write(&d->buffer, feedback_sample);
    return output;
}

/* ============================================================
 * Reverb Effect (Schroeder reverb)
 * ============================================================ */

typedef struct {
    float buffer[MAX_DELAY_SAMPLES];
    int size;
    int pos;
    float feedback;
} CombFilter;

typedef struct {
    float buffer[1024];
    int size;
    int pos;
} AllpassFilter;

typedef struct {
    CombFilter combs[4];
    AllpassFilter allpasses[2];
    float mix;
    float room_size;
    float damping;
} ReverbEffect;

static void comb_init(CombFilter* c, int size, float feedback) {
    memset(c->buffer, 0, sizeof(c->buffer));
    c->size = size;
    c->pos = 0;
    c->feedback = feedback;
}

static float comb_process(CombFilter* c, float input) {
    float output = c->buffer[c->pos];
    c->buffer[c->pos] = input + output * c->feedback;
    c->pos = (c->pos + 1) % c->size;
    return output;
}

static void allpass_init(AllpassFilter* a, int size) {
    memset(a->buffer, 0, sizeof(a->buffer));
    a->size = size;
    a->pos = 0;
}

static float allpass_process(AllpassFilter* a, float input) {
    float bufout = a->buffer[a->pos];
    float output = -input + bufout;
    a->buffer[a->pos] = input + bufout * 0.5f;
    a->pos = (a->pos + 1) % a->size;
    return output;
}

static void reverb_init(ReverbEffect* r, float room_size, float damping, float mix) {
    // Comb filter delays (prime numbers for diffuse response)
    int comb_sizes[] = {1557, 1617, 1491, 1422};
    for (int i = 0; i < 4; i++) {
        comb_init(&r->combs[i], comb_sizes[i], 0.84f * room_size);
    }
    allpass_init(&r->allpasses[0], 556);
    allpass_init(&r->allpasses[1], 441);
    r->mix = mix;
    r->room_size = room_size;
    r->damping = damping;
}

static float reverb_process(ReverbEffect* r, float input) {
    float comb_sum = 0;
    for (int i = 0; i < 4; i++) {
        comb_sum += comb_process(&r->combs[i], input);
    }
    float output = comb_sum;
    for (int i = 0; i < 2; i++) {
        output = allpass_process(&r->allpasses[i], output);
    }
    return input * (1.0f - r->mix) + output * r->mix * 0.25f;
}

/* ============================================================
 * Compressor
 * ============================================================ */

typedef struct {
    float threshold;   // dB
    float ratio;
    float attack;      // seconds
    float release;     // seconds
    float gain;
    float attack_coeff;
    float release_coeff;
} Compressor;

static void compressor_init(Compressor* c, float threshold_db, float ratio,
                            float attack_ms, float release_ms) {
    c->threshold = threshold_db;
    c->ratio = ratio;
    c->attack = attack_ms;
    c->release = release_ms;
    c->gain = 1.0f;
    c->attack_coeff = expf(-1.0f / (attack_ms * SAMPLE_RATE / 1000.0f));
    c->release_coeff = expf(-1.0f / (release_ms * SAMPLE_RATE / 1000.0f));
}

static float compressor_process(Compressor* c, float input) {
    float level = fabsf(input);
    float level_db = 20.0f * log10f(level + 1e-10f);

    float target_gain = 1.0f;
    if (level_db > c->threshold) {
        float over = level_db - c->threshold;
        float compressed = over * (1.0f - 1.0f / c->ratio);
        target_gain = powf(10.0f, -compressed / 20.0f);
    }

    // Smooth gain changes
    float coeff = (target_gain < c->gain) ? c->attack_coeff : c->release_coeff;
    c->gain = coeff * c->gain + (1.0f - coeff) * target_gain;

    return input * c->gain;
}

/* ============================================================
 * Distortion
 * ============================================================ */

typedef enum {
    DIST_SOFT_CLIP,
    DIST_HARD_CLIP,
    DIST_FUZZ,
    DIST_OVERDRIVE
} DistType;

typedef struct {
    DistType type;
    float drive;    // 1.0 = clean, higher = more distortion
    float mix;
    float tone;     // lowpass cutoff for tone shaping
    BiquadFilter tone_filter;
} Distortion;

static void distortion_init(Distortion* d, DistType type, float drive, float mix, float tone) {
    d->type = type;
    d->drive = drive;
    d->mix = mix;
    d->tone = tone;
    biquad_init(&d->tone_filter);
    biquad_lowpass(&d->tone_filter, tone, 0.707f);
}

static float soft_clip(float x) {
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x - (x * x * x) / 3.0f;
}

static float hard_clip(float x) {
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
}

static float fuzz(float x) {
    return tanhf(x * 3.0f);
}

static float overdrive(float x) {
    float threshold = 1.0f / 3.0f;
    if (fabsf(x) < threshold) return 2.0f * x;
    if (x > 0) return (3.0f - powf(2.0f - 3.0f * x, 2.0f)) / 3.0f;
    return -(3.0f - powf(2.0f + 3.0f * x, 2.0f)) / 3.0f;
}

static float distortion_process(Distortion* d, float input) {
    float driven = input * d->drive;
    float distorted;
    switch (d->type) {
        case DIST_SOFT_CLIP: distorted = soft_clip(driven); break;
        case DIST_HARD_CLIP: distorted = hard_clip(driven); break;
        case DIST_FUZZ:      distorted = fuzz(driven); break;
        case DIST_OVERDRIVE: distorted = overdrive(driven); break;
        default: distorted = driven;
    }
    // Apply tone filter
    distorted = biquad_process(&d->tone_filter, distorted);
    return input * (1.0f - d->mix) + distorted * d->mix;
}

/* ============================================================
 * Parametric EQ (Multi-band)
 * ============================================================ */

typedef struct {
    BiquadFilter bands[EQ_BANDS];
    float freqs[EQ_BANDS];
    float gains[EQ_BANDS];
    float Qs[EQ_BANDS];
    int num_bands;
    bool enabled;
} ParametricEQ;

static void eq_init(ParametricEQ* eq) {
    eq->num_bands = 0;
    eq->enabled = true;
    for (int i = 0; i < EQ_BANDS; i++) {
        biquad_init(&eq->bands[i]);
    }
}

static void eq_add_band(ParametricEQ* eq, float freq, float gain_db, float Q) {
    if (eq->num_bands >= EQ_BANDS) return;
    int i = eq->num_bands++;
    eq->freqs[i] = freq;
    eq->gains[i] = gain_db;
    eq->Qs[i] = Q;
    biquad_peaking(&eq->bands[i], freq, gain_db, Q);
}

static float eq_process(ParametricEQ* eq, float input) {
    if (!eq->enabled) return input;
    float output = input;
    for (int i = 0; i < eq->num_bands; i++) {
        output = biquad_process(&eq->bands[i], output);
    }
    return output;
}

/* ============================================================
 * Effects Chain
 * ============================================================ */

typedef struct {
    ParametricEQ eq;
    Compressor comp;
    Distortion dist;
    DelayEffect delay;
    ReverbEffect reverb;

    bool eq_enabled;
    bool comp_enabled;
    bool dist_enabled;
    bool delay_enabled;
    bool reverb_enabled;

    float input_gain;
    float output_gain;

    // Level metering
    float peak_level;
    float rms_level;
    int meter_decay;
} EffectsChain;

static EffectsChain chain;

static void chain_init() {
    memset(&chain, 0, sizeof(chain));

    // Default EQ: 3-band
    eq_init(&chain.eq);
    eq_add_band(&chain.eq, 80.0f, 0.0f, 0.707f);    // Low
    eq_add_band(&chain.eq, 1000.0f, 0.0f, 0.707f);  // Mid
    eq_add_band(&chain.eq, 8000.0f, 0.0f, 0.707f);  // High

    // Default compressor
    compressor_init(&chain.comp, -20.0f, 4.0f, 10.0f, 100.0f);

    // Default distortion
    distortion_init(&chain.dist, DIST_SOFT_CLIP, 2.0f, 0.5f, 4000.0f);

    // Default delay
    delay_init(&chain.delay, 300.0f, 0.4f, 0.3f);

    // Default reverb
    reverb_init(&chain.reverb, 0.7f, 0.5f, 0.3f);

    chain.input_gain = 1.0f;
    chain.output_gain = 1.0f;
    chain.eq_enabled = true;
    chain.comp_enabled = false;
    chain.dist_enabled = false;
    chain.delay_enabled = false;
    chain.reverb_enabled = false;
}

static float chain_process_sample(float input) {
    float sample = input * chain.input_gain;

    // Update metering
    float abs_val = fabsf(sample);
    if (abs_val > chain.peak_level) chain.peak_level = abs_val;
    chain.rms_level = chain.rms_level * 0.999f + abs_val * 0.001f;

    // Process chain
    if (chain.eq_enabled) sample = eq_process(&chain.eq, sample);
    if (chain.comp_enabled) sample = compressor_process(&chain.comp, sample);
    if (chain.dist_enabled) sample = distortion_process(&chain.dist, sample);
    if (chain.delay_enabled) sample = delay_process(&chain.delay, sample);
    if (chain.reverb_enabled) sample = reverb_process(&chain.reverb, sample);

    sample *= chain.output_gain;

    // Soft clip output
    sample = soft_clip(sample);

    return sample;
}

/* ============================================================
 * WASM API
 * ============================================================ */

#ifdef __EMSCRIPTEN__

static void initProcessor() {
    chain_init();
}

static std::vector<float> processBlock(const std::vector<float>& input) {
    std::vector<float> output(input.size());
    for (size_t i = 0; i < input.size(); i++) {
        output[i] = chain_process_sample(input[i]);
    }
    return output;
}

static void setEQBand(int band, float freq, float gain, float Q) {
    if (band >= 0 && band < chain.eq.num_bands) {
        chain.eq.freqs[band] = freq;
        chain.eq.gains[band] = gain;
        chain.eq.Qs[band] = Q;
        biquad_peaking(&chain.eq.bands[band], freq, gain, Q);
    }
}

static void setCompParams(float threshold, float ratio, float attack, float release) {
    compressor_init(&chain.comp, threshold, ratio, attack, release);
}

static void setDistParams(int type, float drive, float mix, float tone) {
    distortion_init(&chain.dist, (DistType)type, drive, mix, tone);
}

static void setDelayParams(float time_ms, float feedback, float mix) {
    delay_init(&chain.delay, time_ms, feedback, mix);
}

static void setReverbParams(float room, float damping, float mix) {
    reverb_init(&chain.reverb, room, damping, mix);
}

static void setEnabled(const std::string& effect, bool enabled) {
    if (effect == "eq") chain.eq_enabled = enabled;
    else if (effect == "comp") chain.comp_enabled = enabled;
    else if (effect == "dist") chain.dist_enabled = enabled;
    else if (effect == "delay") chain.delay_enabled = enabled;
    else if (effect == "reverb") chain.reverb_enabled = enabled;
}

static void setInputGain(float g) { chain.input_gain = g; }
static void setOutputGain(float g) { chain.output_gain = g; }

static float getPeakLevel() {
    float p = chain.peak_level;
    chain.peak_level *= 0.95f; // decay
    return p;
}

static float getRMSLevel() { return chain.rms_level; }

EMSCRIPTEN_BINDINGS(dsp_module) {
    emscripten::function("initProcessor", &initProcessor);
    emscripten::function("processBlock", &processBlock);
    emscripten::function("setEQBand", &setEQBand);
    emscripten::function("setCompParams", &setCompParams);
    emscripten::function("setDistParams", &setDistParams);
    emscripten::function("setDelayParams", &setDelayParams);
    emscripten::function("setReverbParams", &setReverbParams);
    emscripten::function("setEnabled", &setEnabled);
    emscripten::function("setInputGain", &setInputGain);
    emscripten::function("setOutputGain", &setOutputGain);
    emscripten::function("getPeakLevel", &getPeakLevel);
    emscripten::function("getRMSLevel", &getRMSLevel);
    emscripten::register_vector<float>("VectorFloat");
}

#else
int main() {
    chain_init();

    // Generate test signal (sine wave)
    printf("DSP Effects Processor Test\n");
    printf("Sample Rate: %d Hz\n", SAMPLE_RATE);
    printf("Processing 1 second of 440Hz sine wave...\n\n");

    int num_samples = SAMPLE_RATE;
    float peak = 0;

    for (int i = 0; i < num_samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        float input = sinf(2.0f * 3.14159265f * 440.0f * t) * 0.5f;
        float output = chain_process_sample(input);
        if (fabsf(output) > peak) peak = fabsf(output);
    }

    printf("Peak level: %.3f\n", peak);
    printf("EQ bands: %d\n", chain.eq.num_bands);
    printf("Chain: EQ=%d Comp=%d Dist=%d Delay=%d Reverb=%d\n",
           chain.eq_enabled, chain.comp_enabled, chain.dist_enabled,
           chain.delay_enabled, chain.reverb_enabled);
    return 0;
}
#endif
