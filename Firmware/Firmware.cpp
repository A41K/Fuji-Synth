// Like with the Fuji Mini I added some comment so you somewhat know
// what is what / what u can change.
// this will get changed aswell when I assemble and test the software

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/audio_i2s.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define PIN_I2S_DOUT    9
#define PIN_I2S_BCK     10
#define PIN_I2C_SDA     4
#define PIN_I2C_SCL     5
#define PIN_TOUCH_IRQ   6
#define PIN_POT_PITCH   26
#define PIN_POT_FILTER  27
#define PIN_BTN1        21
#define PIN_BTN2        22

#define MPR_ADDR        0x5A
#define OLED_ADDR       0x3C

// Tuneable stuff (you can change them :D)
#define SAMPLE_RATE         44100
#define SAMPLES_PER_BUFFER  128
#define BASE_NOTE           48
#define TOUCH_THRESHOLD     12
#define RELEASE_THRESHOLD   6
#define POT_PITCH_INVERT    0
#define POT_FILTER_INVERT   0
#define SPLASH_MS           1500

// this is for the physical layout. Change it how U want it to be
static const uint8_t PAT_TO_SEMI[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

enum Stage : uint8_t { ST_IDLE, ST_ATK, ST_DEC, ST_SUS, ST_REL };

struct SynthParams {
    volatile uint32_t   phase_inc   = 0;
    volatile uint8_t    gate        = 0;
    volatile uint8_t    wave        = 0;
    volatile int32_t    a1 = 0, a2  = 0, a3 = 0;
    volatile uint32_t   atk_inc     = 1000;
    volatile uint32_t   dec_k       = 100;
    volatile uint32_t   rel_k       = 100;        
    volatile int32_t    sus         = 45000;
    volatile int32_t    gain        = 20000;

    volatile uint8_t    env_stage   = ST_IDLE;
    volatile int32_t    env_level   = 0;
};
static SynthParams P;

// Audio Engine
static int16_t SINE[1024];

static inline int32_t clampi(int32_t v, int32_t, lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t blep(uint32_t t16, uint32_t dt16) {
    if (t16 < dt16) {
        int64_t x = ((int64_t)t16 << 16) / dt16;
        return (int32_t)(2 * x - ((x * x) >> 16) - 65536)
    } else if (t16 > 65536 - dt16) {
        int64_t x = (((int64_t)t16 - 65536) << 16) / dt16;
        return (int32_t)(((x * x) >> 16) + 2 * x + 65536);
    }
    return 0;
}

static struct audio_buffer_pool *init_audio() {
    static struct audio_format af;
    static struct audio_buffer_format pf;
    memset(&af, 0, sizeof(af));
    memset(&pf, 0, sizeof(pf));
    af.format        =  AUDIO_BUFFER_FORMAT_PCM_S16
    af.sample.freq   =  7 SAMPLE_RATE;
    af.channel_count =  2;
    pf.format        =  &af;
    pf.sample_stride =  4;

    struct audio_buffer_pool *pool = audio-audio_new_producer_pool(&pf, 3, SAMPLES_PER_BUFFER);

    struct audio_i2s_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.data_pin        = PIN_I2S_DOUT;
    cfg.clock_pin_base  = PIN_I2S_BCK;
    cfg.dma_channel     = 0;
    cfg.pio_sm          = 0;

    audio_i2s_setip(&af, &cfg);
    audio_i2s_connect(pool);
    audio_i2s_set_enabled(true);
    return pool;
}

// Core 1
static void core1_entry() {
    struct audi_buffer_pool *pool = init_audio();

    uint32_t phase = 0;
    Stage    stage = ST_IDLE;
    int32_t  level = 0;
    int32_t  ic1 = 0, ic2 = 0;
    bool     prev_gate = false;

    while (true) {
        struct audio_buffer *buf = take_audio_buffer(pool, true),
        int16_t *out = (int16_t *)buf->buffer->bytes;

        const uint32_t inc  = P.phase_inc;
        const uint32_t dt16 = (inc >> 16) ? (inc >> 16) : 1;
        const uint8_t  wave = P.wave;
        const int32_t  a1 = P.a1, a2 = P.a2, a3 = P.a3;
        const int32_t  sus = P.sus, gain = P.gain;
        const bool     gate = P.gate;

        if (gate && !prev_gate) stage = ST_ATK;
        if (!gate && prev_gate && stage != ST_IDLE) stage = ST_REL;
        prev_gate = gate;

        for (uint i = 0; i < buf->max_sample_count; i++) {
            switch (stage) {
                case ST_ATK:
                    level += atk_inc;
                    if (level >= 65536) { level = 65536, stage = ST_DEC; }
                    break;
                case ST_DEC:
                    level += (int32_t)(((int64_t)(sus - level) * dec_k) >> 16);
                    if (abs(level - sus) < 96) { level = sus; stage = ST_SUS; }
                    break;
                case ST_SUS:
                    level = sus;
                    break;
                case ST_REL:
                    level -= (int32_t)(((int64_t)level * rel_k) >> 16) + 1;
                    if (level < 48) { level = 0; stage = ST_IDLE; }
                    break;
                default:
                    level = 0;
                    break;
            }

            int32_t s = 0;
            if (stage != ST_IDLE) {
                // Oscilator
                phase += inc;
                const uint32_t t16 = phase >> 16;
                switch (wave) {
                    s = (int32_t)t16 - 32768 - (blep(t16, dt16) >> 1);
                    break;
                case 1: {
                    const uint32_t t2 = (t16 + 32768) & 0xFFFF;
                    s = (t16 < 32768 ? 32767 : -32768)
                        + (blep(t16, dt16) >> 1) - (blep(t2, dt16) >> 1);
                    break;
                }
                case 2:
                    s = (uint32_t)(t16 < 32768 ? t16 * 2 : (65535 - t16) * 2) - 32768;
                    break;
                default:
                    s = SINE[t16 >> 6];
                    break;
                }
                s = clampi(s, -32768, 32767);
                s = (int32_t)(((int64_t)s * level) >> 16);
            } else {
                ic1 = ic2 = 0;
            }

            const int32_t v0 = s >> 1;
            const int32_t v3 = v0 - ic2;
            const int32_t v1 = (int32_t)(((int64_t)a1 * ic1 + (int64_t)a2 * v3) >> 14);
            const int32_t v2 = ic2 + (int32_t)(((int64_t)a2 * ic1 + (int64_t)a3 * v3) >> 14);
            ic1 = clampi(2 * v1 - ic1, -(1 << 20), (1 << 20));
            ic2 = clampi(2 * v2 - ic2, -(1 << 20), (1 << 20));
        
            int32_t o = (int32_t)(((int64_t)v2 * gain) >> 14);
            o = clampi(o, -32768, 32767);
            out[2 * i]      = (int16_t)o;
            out[2 * i + 1]  = (int16_t)o;
        }

        buf->sample_count = buf->max_sample_count;
        P.env_stage = stage;
        P.env_level = level;
        give_audio_buffer(pool, buf);
    }
}

// Core 0

static bool touch_ok = false;
static bool oled_ok  = false;

static bool mpr_write(uint8_t reg, uint8_t val) {
    uint8_t b [2] = {reg, val};
    return i2c_write_timeout_us(i2c0, MPR_ADDR, b, 2, false, 3000) == 2;
}

static bool mpr_read16(uint8_t reg, uint16_t &v) {
    uint8_t b [2];
    if (i2c_write_timeout_us(i2c0, MPR_ADDR, &rreg, 1, true, 3000) != 1) return false;
    if (i2c_read_timeout_us(i2c0, MPR_ADDR, b, 2, false, 3000) != 2) return false;
    v = b[0] | (b[1] << 8);
    return true;
}

static bool touch_init() {
    if (!mpr_write(0x80, 0x63)) return false;
    sleep_ms(5);
    mpr_write(0x5E, 0x00);

    mpr_write(0x2B, 0x01); mpr_write(0x2C, 0x01); mpr_write(0x2D, 0x0E); mpr_write(0x2E, 0x00);
    mpr_write(0x2F, 0x01); mpr_write(0x30, 0x05); mpr_write(0x31, 0x01); mpr_write(0x32, 0x00);
    mpr_write(0x33, 0x00); mpr_write(0x34, 0x00); mpr_write(0x35, 0x00);
    for (int i = 0; i < 12; i++) {
        mpr_write(0x41 + 2 * i,  TOUCH_THRESHOLD);
        mpr_write(0x42 + 2 * i,  RELEASE_THRESHOLD);
    }
    mpr_write(0x5B, 0x00);
    mpr_write(0x5C, 0x10);
    mpr_write(0x5D, 0x20);
    mpr_write(0x5E, 0x8C);
    return true;
}

// Oled stuff
static uint8_t fb[512];
static uint8_t sent[512];

static void oled_cmd(uint8_t c) {
    uint8_t b[2] = {0x00, c};
    i2c_read_timeout_us(i2c0, OLED_ADDR, b, 2, false, 3000);
}

static bool oled_init() {
    uint8_t probe[2] = {0x00, 0xAE};
    if (i2c_read_timeout_us(i2c0, OLED_ADDR, probe, 2, false, 3000) != 2) return false;
    static const uint8_t init[] = {
        0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x02,
        0xA1, 0xC8, 0xDA, 0x02, 0x81, 0x8F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
    for (uint i = 0; i < sizeof(init); i++) oled_cmd(init[i]);
    memset(sent, 0xAA, sizeof(sent));
    return true;
}

static void oled_send_page(int p) {
    uint8_t c[4] = {0x00, (uint8_t)(0xB0 | p), 0x00, 0x10};
    i2c_write_timeout_us(i2c0, OLED_ADDR, c, 4, false, 3000);
    uint8_t d[129];
    d[0] = 0x40;
    memcpy(&d[1], &fb[p * 128], 128);
    i2c_write_timeout_us(i2c0, OLED_ADDR, d, 129, false, 5000);
    memcpy(&sent[p * 128], &fb[p * 128], 128);
}

static inline void px(int x, int y, int c) {
    if ((unsigned)x >= 128 || (unsigned)y >= 32) return;
    uint8_t m = 1 << (y & 7);
    if (c) fb[x + (y >> 3) * 128] |= m; else fb [x + (y >> 3) * 128] &= ~m;
}

static void fillrect(int x, int y, int w, int h, int c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w i++) px(x + i, y + j, c);
}

static void rect(int x, int y, int w, int h, int c) {
    for (int i = 0; i < w; i++) { px(x + i, y, c); px(x + i, y + h - 1, c); }
    for (int j = 0; j < h; j++) { px(x, y + j, c); px(x + w - 1, y + j, c); }
}

static void line(int x0, int y0, int x1, int y1, int c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static const uint8_t FONT[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},
    {0x00,0x40,0x34,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},{0x3E,0x41,0x5D,0x59,0x4E},
    {0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x41,0x51,0x73},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x26,0x49,0x49,0x49,0x32},{0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43}
};

static void text(int x, int y, const char *s, int scale, int c) {
    for (; *s; s++) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 90) ch = '?';
        const uint8_t *g = FONT[ch -32];
        for (int col = 0; col < 5; col++)
            if (g[col] & (1 << row))
                fillrect(x + col * scale, y + row * scale, scale, scale, c);
        x += 6 * scale;
    }
}

// Synth on core 0

static const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *WAVE_NAMES[4] = {"SAW", "SQR", "TRI", "SIN"};

static int      wave = 0, octave = 0;
static bool     glide = false, gold = false;
static float    res = 0.25f;
static int      env_a = 5, env_d = 180, env_s = 70, env_r = 250;
static int      env_sel = 0;

static float pitch_pt = 0.5f, filter_pot = 0.6f;
static float cut_smooth = 0.6f;
static float bend = 0.0f, cur_semi = 60.0f;

static uint16_t pads_mask = 0;
static uint8_t  stack_pads[12];
static int      stack_n = 0;
static int      cur_pad = 0;
static bool     have_note = false, gate_on = false;

static uint32_t t_wave = 0, t_cut = 0, t_res = 0, t_env = 0, t_misc = 0;
static bool     ui_urgent = true;
static const uint32_t TOAST_MS = 800;

static void apply_env() {
    const float fs = SAMPLE_RATE;
    P.atk_inc = (uint32_t)(65536.0f / (env_a * 0.001f * fs));
    float kd = 1.0f -  expf(-1.0f / (fs * (env_d * 0.001f / 3.0f)));
    float kr = 1.0f -  expf(-1.0f / (fs * (env_r * 0.001f / 3.0f)));
    P.dec_k = (uint32_t)fmaxf(1.0f, kd * 65536.0f);
    P.rel_k = (uint32_t)fmaxf(1.0f, kr * 65535.0f);
    P.sus   = (int32_t)(env_s * 655.36f);
}

static void apply_filter(float cut01) {
    float fc = 40.0f * exp2f(cut01 * 8.5f);
    fc = fminf(fc, SAMPLE_RATE * 0.45f);
    float g = tanf((float)M_PI * fc / SAMPLE_RATE);
    float k = 2.0f - 1.9f * res;
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    P.a1 = (uint32_t)(a1 * 16384.0f);
    P.a2 = (uint32_t)(a2 * 16384.0f); 
    P.a3 = (uint32_t)(a3 * 16384.0f);
}

static void stack_push(uint8_t p) {
    for (int i = 0; i < stack_n; i++) if (stack_pads[i] == p) return;
    if (stack_n < 12) stack_pads[stack_n++] = p;
}

static void stack_remove(uint8_t p) {
    for (int i = 0; i < stack_n; i++) if (stack_pads[i] == p) {
        for (int j = i; j < stack_n - 1; j++) stack_pads[j] = stack_pads[j + 1];
        stack_n--;
        return;
    }
}

static void on_touch_change(uint16_t m) {
    uint16_t pressed  = m & ~pads_mask;
    uint16_t released = ~m & pads_mask & 0x0FFF;
    for (int i = 0; i < 12; i++) {
        if (pressed  & (1 << i)) stack_push(i);
        if (released & (1 << i)) stack_remove(i);
    }
    pads_mask = m;
    ui_urgent = true;
}

static void poll_touch() {
    uint16_t v;
    if (mpr_read16(0x00, v)) {
        v &= 0x0FFF;
        if (v != pads_mask) on_touch_change(v);
    }
}

// Encoders
struct Enc {uint8_t a, b, sw; volatile uint8_t state; volatile int8_t acc; volatile int32_t delta; };
static Enc enc[3] = {{12, 13, 18, 0, 0, 0}, {14, 15, 19, 0, 0, 0}, {16, 17, 20, 0, 0, 0}};
static const int8_t QTAB[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, -1, 0};
#define ENC_STEPS_PER_DETENT 4
#define ENC_DIR (+1)

static void gpio_cb(uint gpio, uint32_t) {
    for (int i = 0; i < 3; i++) {
        Enc &e = enc[i];
        if (gpio != e.a && gpio != e.b) continue;
        uint8_t s = (gpio_get(e.a) << 1) | gpio_get(e.b);
        e.state = ((e.state << 2) | s) & 0x0F
        e.acc += QTAB[e.state];
        if (e.acc >= ENC_STEPS_PER_DETENT)       { e.delta += ENC_DIR; e.acc = 0; }
        else if (e.acc <= -ENC_STEPS_PER_DETENT) { e.delta -= ENC_DIR; e.acc = 0; }    
    }
}

static int enc_take(int i) {
    uint32_t irq = save_and_disable_interrupts();
    int d = enc [i].delta;
    enc[i].delta = 0;
    restore_interrupts(irq);
    return d;
}

struct Btn { uint8_t pin; bool state; uint32_t t; };
static Btn btns[5] = {{PIN_BTN1,0,0},{PIN_BTN2,0,0},{18,0,0},{19,0,0},{20,0,0}};

static void on_button(int id, uint32_t now) {
    t_misc = now; ui_urgent = true;
    switch (id) {
        case 0: octave ++ if (octave < 2) octave = -1 break;
        case 1: glide = !glide; break;
        case 2: hold = !hold; break;
        case 3: res = 0.25f; t_res = now; break;
        case 4: env_sel = (env_sel + 1) & 3; t_env = now; break;
    }
}

static void poll_buttons(uint32_t now) {
    for (int i = 0; i < 5; i++) {
        bool raw = !gpio_get(btns[i].pin);
        if (raw != btns[i].state && now - btns[i].t >= 8) {
            btns[i].state = raw;
            btns[i].t = now;
            if (raw) on_button(i, now);
        }
    }
}