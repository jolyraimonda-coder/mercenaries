/* WORLD IN FLAMES v1.0.0 - a code mod for Mercenaries Recompiled 1.1.1
 * (Mod Manager / Mod API v2 + Native Effects / MercFx).
 *
 * Recreates the Mercenaries 2 "World in Flames" opening-cutscene nuke on the
 * bunker buster, fuel-air bomb and cruise missile strikes:
 *  - blinding whiteout flash (8 stacked screen-filling passes) + white-hot core;
 *  - fire-dominant mushroom cloud: flat-topped orange fire dome, warm
 *    red-brown smoke, wide dark skirt, thick fiery stem, bobbing;
 *  - giant soft yellow sky glow (the cutscene's orange sky);
 *  - low orange fire wave hugging the terrain, sweeping to the blast radius;
 *  - grey dust blanket drifting over the streets;
 *  - shockwave rings, ground scorch, earthquake shake, slow motion,
 *    layered nuke audio (a_strike.bb_explosion) with the default blast sound
 *    suppressible.
 *
 * Built on the verified MercFx calling pattern: particles are registered once
 * at attach (sprites at most 100 units, alpha at most 128, as the framework
 * requires), prepared lazily inside the impact dispatch, emitted by a
 * registered simulation timeline, and reset by the framework's world-reset
 * notification. Without Native Effects every bomb keeps its vanilla blast.
 *
 * Build (64-bit Windows DLL), with mercenaries_mod.h and mercfx.h beside this file:
 *   cl /nologo /W4 /O2 /MT /LD world_in_flames.c /I . /Fe:world_in_flames.dll /link /MACHINE:X64 kernel32.lib libucrt.lib libvcruntime.lib
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "mercenaries_mod.h"
#include "mercfx.h"

/* ------------------------------------------------------------------ tuning */

#define MOD_NAME            "WORLD IN FLAMES"
#define MOD_VERSION         "1.0.0"
#define MOD_INI             "mods\\world_in_flames.ini"
#define MOD_SECTION         "WorldInFlames"
#define MOD_OWNER           "world_in_flames"

#define EFFECT_NAME         "world_in_flames.main"

/* Retail sprite helpers for the whiteout flash (verified addresses). */
#define SPRITE_BEGIN_FN     0x0020A0D0u
#define SPRITE_QUAD_FN      0x0020C980u

/* The game's cached textures, as the MercFx framework reads them. */
#define TEX_FLASH_HASH      0x88e27c45u
#define TEX_EXPLOSION_HASH  0x02bb1fe0u
#define TEX_SMOKE_HASH      0xc272d7afu

#define NUK_TIMELINES       4u
#define FLASH_PASSES        8u    /* stacked screen-filling flashes */
#define MAX_BRUSHES         96u

/* Trigger templates: every heavy strike becomes the nuke. */
static const char *const triggers[] = {
    "template_amm_bunkerbuster",
    "template_amm_bunkerbuster0",
    "template_amm_bunkerbusterbomb",
    "template_amm_fuelairbomb",
    "template_amm_fuelairbomb0",
    "template_amm_cruisemissile",
    "template_amm_cruisemissile0",
};
#define TRIGGER_COUNT (sizeof(triggers) / sizeof(triggers[0]))
#define HASH_SLOTS 16u

/* Particle types. */
enum { P_FLASH, P_FIRE, P_GLOW, P_SMOKE, P_DUST, P_PLUME, P_WAVE, P_GROUND, P_COUNT };

typedef struct Timeline {
    int active;
    int mega;              /* MEGA yield: taller cap, longer show */
    float pos[3];
    float age, duration;
    float next_cap, next_stem, next_wave, next_dust, next_ring, next_core, next_glow;
    unsigned seed;
    uint32_t shock;        /* shockwave cue handle, plays once */
} Timeline;

/* ------------------------------------------------------------------- state */

static const MercenariesModHost *host;
static const MercFxApi *fx;

static unsigned cfg_master   = 1;  /* MASTER */
static unsigned cfg_yield    = 2;  /* 1 STRIKE, 2 TSAR, 3 MEGA */
static unsigned cfg_wave     = 1;  /* FIRE WAVE */
static unsigned cfg_quake    = 2;  /* 0 off, 1 strong, 2 extreme */
static unsigned cfg_audio    = 2;  /* 0 off, 1 layered, 2 nuke only */
static unsigned cfg_whiteout = 1;  /* WHITEOUT */
static unsigned cfg_debug    = 0;  /* DEBUG LOG */
static float view_range      = 4096.0f;

static int attach_state;          /* 0 untried, 1 trying, 2 attached, 3 given up */
static unsigned attach_attempts;
static unsigned module_misses;

static uint32_t sound_primary;    /* a_strike.bb_explosion */

static uint32_t trigger_hash[HASH_SLOTS];
static unsigned char bound_flags[HASH_SLOTS];
static unsigned bound_count;
static unsigned rebind_timer;

static Timeline timelines[NUK_TIMELINES];
static unsigned timeline_cursor;
static uint32_t particles[P_COUNT];

static uint32_t flash_texture;
static float flash_time = -1.0f;
static float flash_hold, flash_fade;
static unsigned brushes_this_frame, brushes_last_frame;

static int have_begin, have_quad;

static double world_time;

/* ------------------------------------------------------------------ helpers */

static uint32_t fnv1a_casefold(const char *text)
{
    uint32_t hash = 0x811C9DC5u;
    while (*text) hash = (hash ^ ((unsigned char)*text++ | 0x20u)) * 16777619u;
    return hash;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

#define LOG(...) do { if (host && host->log) host->log("world_in_flames", __VA_ARGS__); } while (0)
#define DLOG(...) do { if (cfg_debug && host && host->log) host->log("wif_debug", __VA_ARGS__); } while (0)

static float rand_frac(unsigned *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return (float)(*seed & 0x00FFFFFFu) / 16777216.0f;
}

static float finite3(const float *v) { return (float)(v[0]) + v[1] + v[2]; }

/* ------------------------------------------------------------- particle setup */

/* All sizes stay inside the framework's (0,100] sprite bound and every alpha
   inside <=128, so registration can never be rejected. */
static void register_type(unsigned index, const MercFxParticle *spec)
{
    if (!fx || !fx->register_particle) return;
    particles[index] = fx->register_particle(spec);
    DLOG("particle %u registered as %u", index, particles[index]);
}

static void register_particles(void)
{
    MercFxParticle s;
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.max_distance = view_range;

    /* FLASH: blinding white-yellow core of the fireball. */
    s.texture_hash = TEX_FLASH_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x80FFFFFFu; s.colors[1] = 0x60FFF2D0u; s.colors[2] = 0x00FFEFC0u;
    s.widths[0] = 30.0f;  s.heights[0] = 30.0f;
    s.widths[1] = 70.0f;  s.heights[1] = 70.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.2f;
    s.y_acceleration = 2.0f; s.friction = 0.5f;
    s.lifetime = 0.6f; s.lifetime_random = 0.2f;
    register_type(P_FLASH, &s);

    /* FIRE: bright yellow-white fading to orange (the cutscene dome). */
    s.texture_hash = TEX_EXPLOSION_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x80FFFFD8u; s.colors[1] = 0x70FFC840u; s.colors[2] = 0x30E06000u;
    s.widths[0] = 30.0f;  s.heights[0] = 30.0f;
    s.widths[1] = 70.0f;  s.heights[1] = 70.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.25f;
    s.y_acceleration = 6.0f; s.friction = 0.3f;
    s.lifetime = 5.5f; s.lifetime_random = 1.5f;
    s.random_scale = 0.4f; s.random_acceleration = 3.0f;
    register_type(P_FIRE, &s);

    /* GLOW: giant soft yellow ambient light (the orange sky). */
    s.texture_hash = TEX_FLASH_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x28FFEE90u; s.colors[1] = 0x18FFD840u; s.colors[2] = 0x00C08020u;
    s.widths[0] = 60.0f;  s.heights[0] = 60.0f;
    s.widths[1] = 90.0f;  s.heights[1] = 90.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 1.0f; s.friction = 0.2f;
    s.lifetime = 6.0f; s.lifetime_random = 1.0f;
    register_type(P_GLOW, &s);

    /* SMOKE: warm red-brown, near-black at the end (the cutscene cap). */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x804A2C20u; s.colors[1] = 0x60341E16u; s.colors[2] = 0x0018100Cu;
    s.widths[0] = 50.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 70.0f;  s.heights[1] = 55.0f;
    s.widths[2] = 100.0f; s.heights[2] = 80.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 4.0f; s.friction = 0.2f;
    s.lifetime = 9.0f; s.lifetime_random = 2.0f;
    s.random_scale = 0.35f; s.random_acceleration = 2.0f;
    s.wind = 1.5f;
    register_type(P_SMOKE, &s);

    /* DUST: tan-grey ground dust ring. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x60BBAA88u; s.colors[1] = 0x40998877u; s.colors[2] = 0x20776655u;
    s.widths[0] = 40.0f;  s.heights[0] = 15.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 35.0f;
    s.widths[2] = 100.0f; s.heights[2] = 55.0f;
    s.time_b = 0.4f;
    s.y_acceleration = 1.5f; s.friction = 0.4f;
    s.lifetime = 8.0f; s.lifetime_random = 2.0f;
    register_type(P_DUST, &s);

    /* PLUME: dark red-brown stem smoke, slow rise. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x8042261Cu; s.colors[1] = 0x602E1A12u; s.colors[2] = 0x0016100Cu;
    s.widths[0] = 25.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 85.0f;
    s.widths[2] = 95.0f;  s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 5.0f; s.friction = 0.25f;
    s.lifetime = 10.0f; s.lifetime_random = 2.0f;
    register_type(P_PLUME, &s);

    /* WAVE: bright orange fire wall hugging the terrain. */
    s.texture_hash = TEX_EXPLOSION_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x70FFB030u; s.colors[1] = 0x60FF7010u; s.colors[2] = 0x30A02800u;
    s.widths[0] = 25.0f;  s.heights[0] = 25.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 60.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 3.0f; s.friction = 0.35f;
    s.lifetime = 3.5f; s.lifetime_random = 1.0f;
    s.random_scale = 0.4f;
    register_type(P_WAVE, &s);

    /* GROUND: dark dust blanket drifting over the streets. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x60554030u; s.colors[1] = 0x48382A20u; s.colors[2] = 0x10201A14u;
    s.widths[0] = 40.0f;  s.heights[0] = 12.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 20.0f;
    s.widths[2] = 100.0f; s.heights[2] = 30.0f;
    s.time_b = 0.35f;
    s.y_acceleration = 1.0f; s.friction = 0.3f;
    s.lifetime = 14.0f; s.lifetime_random = 4.0f;
    s.wind = 3.0f;
    register_type(P_GROUND, &s);
}

static void emit(unsigned type, const float p[3], float vx, float vy, float vz, unsigned *seed)
{
    float velocity[3];
    float spin;
    if (!particles[type] || !fx || !fx->emit_particle) return;
    velocity[0] = vx; velocity[1] = vy; velocity[2] = vz;
    spin = (rand_frac(seed) - 0.5f) * 2.0f;
    fx->emit_particle(particles[type], p, velocity, spin, 1.0f);
}

/* ---------------------------------------------------------------- timeline */

static Timeline *grab_timeline(void)
{
    unsigned i;
    Timeline *candidate;
    for (i = 0; i < NUK_TIMELINES; ++i) {
        candidate = &timelines[timeline_cursor];
        timeline_cursor = (timeline_cursor + 1u) % NUK_TIMELINES;
        if (!candidate->active) return candidate;
    }
    return &timelines[0];   /* all busy: reuse the oldest */
}

static float cap_height(const Timeline *t) { return t->mega ? 100.0f : 80.0f; }
static float cap_radius(const Timeline *t) { return t->mega ? 85.0f : 70.0f; }

/* Mushroom cap: flat-topped fire dome, red-brown smoke gaps, wide dark skirt. */
static void emit_cap(Timeline *t, float dt)
{
    const float deck = cap_height(t) + sinf(t->age * 1.05f) * 6.0f;
    const float unfurl = t->age < 10.0f ? t->age / 10.0f : 1.0f;
    unsigned j;

    t->next_cap -= dt;
    if (t->next_cap > 0.0f) return;
    t->next_cap = 0.08f;

    for (j = 0; j < 24; ++j) {
        float angle = 6.2832f * rand_frac(&t->seed);
        float radius = cap_radius(t) * unfurl * sqrtf(rand_frac(&t->seed));
        float p[3];
        p[0] = t->pos[0] + cosf(angle) * radius;
        p[2] = t->pos[2] + sinf(angle) * radius;
        if (j < 14) {
            /* fire-dominant glowing dome */
            p[1] = t->pos[1] + deck - 10.0f + 20.0f * rand_frac(&t->seed);
            emit(P_FIRE, p, cosf(angle) * 3.0f, (t->age < 5.0f) ? 8.0f : 1.2f,
                 sinf(angle) * 3.0f, &t->seed);
        } else if (j < 20) {
            /* red-brown smoke between the flames */
            p[1] = t->pos[1] + deck - 8.0f + 6.0f * rand_frac(&t->seed);
            emit(P_SMOKE, p, cosf(angle) * 3.0f, (t->age < 5.0f) ? 8.0f : 1.0f,
                 sinf(angle) * 3.0f, &t->seed);
        } else {
            /* wide dark skirt under the head */
            radius = cap_radius(t) * 1.35f * unfurl * sqrtf(rand_frac(&t->seed));
            p[0] = t->pos[0] + cosf(angle) * radius;
            p[2] = t->pos[2] + sinf(angle) * radius;
            p[1] = t->pos[1] + deck - 14.0f + 4.0f * rand_frac(&t->seed);
            emit(P_SMOKE, p, cosf(angle) * 2.0f, 0.8f, sinf(angle) * 2.0f, &t->seed);
        }
    }
}

/* Thick fiery stem: dark plumes with fire patches burning inside. */
static void emit_stem(Timeline *t, float dt)
{
    unsigned j;

    t->next_stem -= dt;
    if (t->next_stem > 0.0f) return;
    t->next_stem = 0.12f;

    for (j = 0; j < 3; ++j) {
        float angle = 6.2832f * rand_frac(&t->seed);
        float radius = 8.0f + 10.0f * rand_frac(&t->seed);
        float p[3];
        p[0] = t->pos[0] + cosf(angle) * radius;
        p[1] = t->pos[1] + 3.0f + (cap_height(t) * 0.9f) * rand_frac(&t->seed);
        p[2] = t->pos[2] + sinf(angle) * radius;
        emit(P_PLUME, p, cosf(angle) * 0.5f, 6.0f + 4.0f * rand_frac(&t->seed),
             sinf(angle) * 0.5f, &t->seed);
    }
    for (j = 0; j < 3; ++j) {
        float angle = 6.2832f * rand_frac(&t->seed);
        float radius = 6.0f + 9.0f * rand_frac(&t->seed);
        float top_bias = (j < 2) ? 0.85f : 0.45f;
        float p[3];
        p[0] = t->pos[0] + cosf(angle) * radius;
        p[1] = t->pos[1] + 3.0f + (cap_height(t) * top_bias) * rand_frac(&t->seed);
        p[2] = t->pos[2] + sinf(angle) * radius;
        emit(P_FIRE, p, 0.0f, 5.0f, 0.0f, &t->seed);
    }
}

/* Low orange fire wave hugging the terrain (the cutscene's burning streets). */
static void emit_wave(Timeline *t, float dt)
{
    const float radius = 30.0f + t->age * 130.0f;
    unsigned j;

    if (!cfg_wave) return;
    t->next_wave -= dt;
    if (t->next_wave > 0.0f) return;
    t->next_wave = 0.10f;

    for (j = 0; j < 24; ++j) {
        float angle = (6.2832f * (float)j) / 24.0f + rand_frac(&t->seed) * 0.12f;
        float jitter = radius + (rand_frac(&t->seed) - 0.5f) * 30.0f;
        float x = t->pos[0] + cosf(angle) * jitter;
        float z = t->pos[2] + sinf(angle) * jitter;
        float terrain = t->pos[1];
        float p[3];
        if (fx && fx->sample_terrain_height) {
            float h;
            if (fx->sample_terrain_height(x, z, &h)) terrain = h;
        }
        p[0] = x; p[2] = z;
        p[1] = terrain + 2.5f;
        emit(P_WAVE, p, cosf(angle) * 30.0f, 4.0f, sinf(angle) * 30.0f, &t->seed);
        p[1] = terrain + 10.0f;
        emit(P_SMOKE, p, cosf(angle) * 12.0f, 2.0f, sinf(angle) * 12.0f, &t->seed);
    }
}

/* Grey dust blanket drifting over the ground (golden-angle spiral). */
static void emit_blanket(Timeline *t, float dt)
{
    unsigned j;
    static unsigned blanket_sample;

    t->next_dust -= dt;
    if (t->next_dust > 0.0f) return;
    t->next_dust = 0.30f;

    for (j = 0; j < 3; ++j) {
        float sample = (float)blanket_sample++;
        float radius = (40.0f + t->age * 90.0f) * sqrtf(fmodf(0.5f + sample * 0.618f, 1.0f));
        float angle = sample * 2.3999632f;
        float x = t->pos[0] + cosf(angle) * radius;
        float z = t->pos[2] + sinf(angle) * radius;
        float terrain = t->pos[1];
        float p[3];
        if (fx && fx->sample_terrain_height) {
            float h;
            if (fx->sample_terrain_height(x, z, &h)) terrain = h;
        }
        p[0] = x; p[2] = z;
        p[1] = terrain + 4.0f;
        emit(P_GROUND, p, cosf(angle) * 8.0f, 3.0f, sinf(angle) * 8.0f, &t->seed);
    }
}

static void __cdecl wif_timeline_tick(float delta_seconds, void *userdata)
{
    unsigned i;
    (void)userdata;
    if (!cfg_master) return;
    for (i = 0; i < NUK_TIMELINES; ++i) {
        Timeline *t = &timelines[i];
        if (!t->active) continue;
        t->age += delta_seconds;

        /* shockwave cue once, when the wave starts rolling */
        if (t->shock && t->age >= 0.9f) {
            if (fx->sound_ready && fx->sound_ready(t->shock))
                fx->play_sound(t->shock, t->pos);
            t->shock = 0;
        }

        /* white-hot core: blinding glow at the center for the first 3 s */
        t->next_core -= delta_seconds;
        if (t->next_core <= 0.0f && t->age < 3.0f) {
            float p[3];
            t->next_core = 0.05f;
            p[0] = t->pos[0];
            p[2] = t->pos[2];
            p[1] = t->pos[1] + 6.0f + 14.0f * rand_frac(&t->seed);
            emit(P_FLASH, p, 0.0f, 0.0f, 0.0f, &t->seed);
        }

        /* ambient sky glow while the dome builds */
        t->next_glow -= delta_seconds;
        if (t->next_glow <= 0.0f && t->age < 8.0f) {
            float p[3];
            t->next_glow = 0.15f;
            p[0] = t->pos[0] + (rand_frac(&t->seed) - 0.5f) * 50.0f;
            p[1] = t->pos[1] + cap_height(t) + 10.0f;
            p[2] = t->pos[2] + (rand_frac(&t->seed) - 0.5f) * 50.0f;
            emit(P_GLOW, p, 0.0f, 2.0f, 0.0f, &t->seed);
        }

        emit_cap(t, delta_seconds);
        emit_stem(t, delta_seconds);
        emit_wave(t, delta_seconds);
        emit_blanket(t, delta_seconds);

        /* rolling shockwave rings while the wave sweeps */
        t->next_ring -= delta_seconds;
        if (t->next_ring <= 0.0f && fx->emit_shockwave) {
            MercFxShockwave shock;
            t->next_ring = 2.0f;
            if (t->age < t->duration * 0.6f) {
                shock.size = sizeof(shock);
                shock.flags = t->age > 1.5f ? MERCFX_SHOCKWAVE_RINGS
                                            : (MERCFX_SHOCKWAVE_REFRACTION | MERCFX_SHOCKWAVE_RINGS);
                shock.radius = 200.0f;
                shock.duration = 3.0f;
                fx->emit_shockwave(t->pos, &shock);
            }
        }

        if (t->age >= t->duration) t->active = 0;
    }
}

static void __cdecl wif_timeline_reset(void *userdata)
{
    (void)userdata;
    memset(timelines, 0, sizeof(timelines));
    flash_time = -1.0f;
}

/* ---------------------------------------------------------------- attach */

static MercFxDecision __cdecl wif_impact(const MercFxImpact *event, void *userdata);

static int try_attach(void)
{
    HMODULE module;
    MercFxGetApi get_api;
    unsigned i, ok_count = 0;

    if (!host || attach_state == 2 || attach_state == 3) return attach_state == 2;
    module = GetModuleHandleW(L"native_effects.dll");
    if (!module) {
        if (++module_misses >= 600) {
            attach_state = 3;
            LOG("native_effects.dll not loaded; WORLD IN FLAMES stays vanilla");
        }
        return 0;
    }
    get_api = (MercFxGetApi)(void *)GetProcAddress(module, "MercFx_GetApi");
    if (!get_api) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("native_effects.dll has no MercFx_GetApi; staying vanilla");
        }
        return 0;
    }
    fx = get_api(MERCFX_API_VERSION);
    if (!fx || fx->version != MERCFX_API_VERSION) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("MercFx API version mismatch; staying vanilla");
        }
        return 0;
    }
    if (fx->size < offsetof(MercFxApi, register_sound) + sizeof(fx->register_sound) ||
        !fx->register_effect || !fx->bind_template || !fx->apply_blast_configured ||
        !fx->emit_shockwave || !fx->request_slowdown || !fx->register_simulation ||
        !fx->add_ground_scorch || !fx->register_particle || !fx->prepare_particle ||
        !fx->emit_particle || !fx->sample_terrain_height) {
        attach_state = 3;
        LOG("MercFx API table incomplete (%u bytes); staying vanilla", fx->size);
        return 0;
    }

    register_particles();
    for (i = 0; i < P_COUNT; ++i) {
        if (particles[i]) LOG("particle %u of %u registered", i + 1u, (unsigned)P_COUNT);
        else LOG("particle %u of %u refused by the framework", i + 1u, (unsigned)P_COUNT);
    }

    if (!fx->register_effect(EFFECT_NAME, wif_impact, NULL)) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("MercFx effect registration failed; staying vanilla");
        }
        return 0;
    }
    attach_state = 2;

    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) {
        unsigned char ok = fx->bind_template(triggers[i], EFFECT_NAME, 1) ? 1u : 0u;
        trigger_hash[i] = fnv1a_casefold(triggers[i]);
        bound_flags[i] = ok;
        if (ok) ++ok_count;
        LOG("bind %u %s: %s", i, triggers[i], ok ? "BOUND" : "not bound (absent or owned)");
    }
    bound_count = ok_count;

    sound_primary = fx->register_sound("a_strike.bb_explosion");
    LOG("blast cue a_strike.bb_explosion -> %u (0 = unavailable)", sound_primary);

    if (!fx->register_simulation(wif_timeline_tick, wif_timeline_reset, NULL))
        LOG("timeline registration failed; the cloud will not build");

    LOG("attached: bound=%u of %u, master=%u, yield=%u, quake=%u, audio=%u, whiteout=%u",
        bound_count, (unsigned)TRIGGER_COUNT, cfg_master, cfg_yield, cfg_quake,
        cfg_audio, cfg_whiteout);
    return 1;
}

/* ------------------------------------------------------------ impact handler */

static void quake_opts(MercFxBlastOptions *options)
{
    if (cfg_quake == 1) { options->shake_duration = 10.0f; options->shake_magnitude = 3.0f; }
    else if (cfg_quake == 2) { options->shake_duration = 20.0f; options->shake_magnitude = 5.0f; }
}

static MercFxDecision __cdecl wif_impact(const MercFxImpact *event, void *userdata)
{
    MercFxImpact *ev = (MercFxImpact *)event;   /* framework-owned copy */
    MercFxBlastOptions options;
    Timeline *t;
    unsigned i, seed;
    int known = 0, audio = 0;
    float p[3];

    (void)userdata;
    if (!cfg_master) return MERCFX_UNHANDLED;
    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i)
        if (event->template_hash == trigger_hash[i] && bound_flags[i]) { known = 1; break; }
    if (!known) return MERCFX_UNHANDLED;

    /* Prepare every particle while the engine context is live: this is the
       only place the retail constructor can run (the MOAB pattern). */
    for (i = 0; i < P_COUNT; ++i)
        if (particles[i] && !fx->prepare_particle(particles[i])) {
            DLOG("particle %u refused to prepare", i);
            return MERCFX_UNHANDLED;
        }

    /* gameplay: one committed blast, scaled by the yield tier */
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.damage_type = MERCFX_BLAST_BUNKERBUSTER;
    options.scorch_radius = 40.0f;
    quake_opts(&options);
    if (cfg_audio == 2 && sound_primary && fx->sound_ready && fx->sound_ready(sound_primary))
        options.suppress_default_sound = 1u;      /* NUKE ONLY */

    ev->splash_damage *= 10.0f;
    ev->splash_force *= 12.0f;
    ev->splash_radius = (cfg_yield == 1) ? 700.0f : 1000.0f;

    if (!fx->apply_blast_configured(event, &options)) {
        LOG("blast rejected by the framework; the vanilla explosion took over");
        return MERCFX_UNHANDLED;
    }

    fx->request_slowdown(MOD_OWNER, 0.5f, 1.2f);
    {
        MercFxShockwave shock;
        shock.size = sizeof(shock);
        shock.flags = MERCFX_SHOCKWAVE_REFRACTION | MERCFX_SHOCKWAVE_RINGS;
        shock.radius = 200.0f;
        shock.duration = 3.0f;
        fx->emit_shockwave(event->position, &shock);
    }

    /* audio: the strike cue over the blast */
    if (cfg_audio != 0 && sound_primary && fx->sound_ready && fx->sound_ready(sound_primary)) {
        fx->play_sound(sound_primary, event->position);
        audio = 1;
    }
    if (cfg_audio != 0 && !audio)
        DLOG("blast cue not ready; the vanilla sound stands");

    /* whiteout flash over the screen */
    if (cfg_whiteout && flash_texture) {
        flash_hold = (cfg_yield == 3) ? 1.2f : 0.8f;
        flash_fade = (cfg_yield == 3) ? 3.0f : 2.0f;
        flash_time = 0.0f;
    }

    /* initial fireball burst at the impact point */
    seed = 0x51ABF00Du ^ (unsigned)(int)finite3(event->position);
    for (i = 0; i < 3; ++i) {
        p[0] = event->position[0];
        p[1] = event->position[1] + 2.0f;
        p[2] = event->position[2];
        emit(P_FLASH, p, 0.0f, 0.0f, 0.0f, &seed);
    }
    for (i = 0; i < 30; ++i) {
        float angle = 6.2832f * rand_frac(&seed);
        float speed = 10.0f + 18.0f * rand_frac(&seed);
        p[0] = event->position[0];
        p[1] = event->position[1] + 2.0f;
        p[2] = event->position[2];
        emit(P_FIRE, p, cosf(angle) * speed, 1.0f * rand_frac(&seed) + 2.0f,
             sinf(angle) * speed, &seed);
    }
    for (i = 0; i < 20; ++i) {
        float angle = 6.2832f * (float)i / 20.0f;
        p[0] = event->position[0] + cosf(angle) * 5.0f;
        p[1] = event->position[1] + 0.75f;
        p[2] = event->position[2] + sinf(angle) * 5.0f;
        emit(P_DUST, p, cosf(angle) * 32.0f, 0.6f, sinf(angle) * 32.0f, &seed);
    }

    /* start the sustained cloud timeline */
    t = grab_timeline();
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->mega = (cfg_yield == 3);
    t->age = 0.0f;
    t->duration = (cfg_yield == 1) ? 25.0f : (cfg_yield == 3) ? 40.0f : 30.0f;
    t->pos[0] = event->position[0];
    t->pos[1] = event->position[1];
    t->pos[2] = event->position[2];
    t->next_cap = 0.0f;
    t->next_stem = 0.05f;
    t->next_wave = 0.02f;
    t->next_dust = 0.30f;
    t->next_ring = 2.0f;
    t->next_core = 0.0f;
    t->next_glow = 0.10f;
    t->seed = 0x57121c47u ^ (unsigned)(int)finite3(event->position) ^ seed;
    t->shock = (cfg_audio == 1 && sound_primary) ? sound_primary : 0;

    LOG("world in flames: yield=%u r=%.0f at %.0f %.0f %.0f",
        cfg_yield, event->splash_radius, event->position[0], event->position[1],
        event->position[2]);
    return MERCFX_REMOVE;
}

/* -------------------------------------------------------------------- hooks */

static void draw_flash(uint32_t brush)
{
    float t = flash_time;
    float alpha, per_brush;
    uint32_t color, begin_args[5], quad_args[8];
    unsigned n, pass;

    if (!flash_texture || !have_begin || !have_quad) return;
    n = brushes_last_frame;
    if (!n || n > MAX_BRUSHES) return;

    if (t < flash_hold) {
        alpha = 1.0f;
    } else {
        float total = t - flash_hold;
        if (total >= flash_fade) { flash_time = -1.0f; return; }
        alpha = powf(1.0f - total / flash_fade, 1.6f);
    }
    per_brush = 1.0f - powf(1.0f - alpha / (float)FLASH_PASSES, 1.0f / (float)n);
    if (per_brush < 0.0f) per_brush = 0.0f;
    color = ((uint32_t)(per_brush * 127.0f + 0.5f) << 24) | 0x00FFFFFFu;

    begin_args[0] = flash_texture;
    begin_args[1] = color;
    begin_args[2] = 0u;
    begin_args[3] = 0u;
    begin_args[4] = 0u;
    quad_args[0] = float_bits(0.0f);
    quad_args[1] = float_bits(0.0f);
    quad_args[2] = float_bits(640.0f);
    quad_args[3] = float_bits(480.0f);
    quad_args[4] = float_bits(0.0f);
    quad_args[5] = float_bits(0.0f);
    quad_args[6] = float_bits(1.0f);
    quad_args[7] = float_bits(1.0f);
    for (pass = 0; pass < FLASH_PASSES; ++pass) {
        host->guest_call(SPRITE_BEGIN_FN, brush, 5u, begin_args);
        host->guest_call(SPRITE_QUAD_FN, brush, 8u, quad_args);
    }
}

static void world_tick(uint32_t delta_bits)
{
    float delta;
    memcpy(&delta, &delta_bits, sizeof(delta));
    if (!(delta > 0.0f) || !isfinite(delta)) delta = 0.0f;

    world_time += delta;
    brushes_last_frame = brushes_this_frame;
    brushes_this_frame = 0;

    if (attach_state != 2 && attach_state != 3) try_attach();

    /* rebind trigger templates every 2 s until all bound */
    if (attach_state == 2 && bound_count < (unsigned)TRIGGER_COUNT) {
        rebind_timer += (unsigned)(delta * 1000.0f);
        if (rebind_timer >= 2000u) {
            unsigned i, ok = 0;
            rebind_timer = 0;
            for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) {
                if (!bound_flags[i] && fx->bind_template(triggers[i], EFFECT_NAME, 1)) {
                    bound_flags[i] = 1;
                    LOG("late bind: %s", triggers[i]);
                }
                if (bound_flags[i]) ++ok;
            }
            bound_count = ok;
        }
    }

    if (flash_time >= 0.0f) {
        flash_time += delta;
        if (flash_time > flash_hold + flash_fade + 1.0f)
            flash_time = -1.0f;
    }
}

static void brush_end(uint32_t brush, int pda, uint32_t vertex_base, uint32_t vertex_first,
                      uint32_t *vertex_end, uint32_t primitive_base, uint32_t primitive_first,
                      uint32_t *primitive_end)
{
    (void)vertex_base; (void)vertex_first; (void)vertex_end;
    (void)primitive_base; (void)primitive_first; (void)primitive_end;
    ++brushes_this_frame;
    if (pda) return;
    if (flash_time >= 0.0f) draw_flash(brush);
}

static void shutdown_hook(void)
{
    if (fx) {
        if (fx->cancel_slowdown) fx->cancel_slowdown(MOD_OWNER);
        if (fx->set_template_enabled) fx->set_template_enabled(triggers[0], 0);
        if (fx->unregister_effect) fx->unregister_effect(EFFECT_NAME);
        if (fx->unregister_simulation) fx->unregister_simulation(wif_timeline_tick, NULL);
    }
    fx = NULL;
}

/* --------------------------------------------------------------- settings */

static unsigned get_master(void)   { return cfg_master; }
static unsigned get_yield(void)    { return cfg_yield; }
static unsigned get_wave(void)     { return cfg_wave; }
static unsigned get_quake(void)    { return cfg_quake; }
static unsigned get_audio(void)    { return cfg_audio; }
static unsigned get_whiteout(void) { return cfg_whiteout; }
static unsigned get_debug(void)    { return cfg_debug; }

static void write_unsigned(const char *key, unsigned value)
{
    if (host && host->write_setting) host->write_setting(MOD_INI, MOD_SECTION, key, value);
}

#define SET_on(name, var) \
    static void set_##var(unsigned v) { cfg_##var = v & 1u; write_unsigned(name, cfg_##var); }

SET_on("Master", master)
SET_on("FireWave", wave)
SET_on("Whiteout", whiteout)
SET_on("DebugLog", debug)

static void set_yield(unsigned v)
{
    if (v < 1u) v = 1u;
    if (v > 3u) v = 3u;
    cfg_yield = v; write_unsigned("Yield", v);
}
static void set_quake(unsigned v)
{
    if (v > 2u) v = 2u;
    cfg_quake = v; write_unsigned("Earthquake", v);
}
static void set_audio(unsigned v)
{
    if (v > 2u) v = 2u;
    cfg_audio = v; write_unsigned("NukeAudio", v);
}

static const char *const on_off[] = {"OFF", "ON"};
static const char *const yield_labels[] = {"", "STRIKE", "TSAR", "MEGA"};
static const char *const quake_labels[] = {"OFF", "STRONG", "EXTREME"};
static const char *const audio_labels[] = {"OFF", "LAYERED", "NUKE ONLY"};

#define SETTING_COUNT 7u

static const MercenariesModSetting settings[SETTING_COUNT] = {
    {"MASTER",       on_off, 2, get_master,    set_master},
    {"YIELD",        yield_labels, 4, get_yield, set_yield},
    {"FIRE WAVE",    on_off, 2, get_wave,      set_wave},
    {"EARTHQUAKE",   quake_labels, 3, get_quake, set_quake},
    {"NUKE AUDIO",   audio_labels, 3, get_audio, set_audio},
    {"WHITEOUT",     on_off, 2, get_whiteout,  set_whiteout},
    {"DEBUG LOG",    on_off, 2, get_debug,     set_debug},
};

static const uint8_t setting_controls[SETTING_COUNT] = {
    MERCENARIES_MOD_CONTROL_SLIDER,
    MERCENARIES_MOD_CONTROL_SLIDER,
    MERCENARIES_MOD_CONTROL_SLIDER,
    MERCENARIES_MOD_CONTROL_SLIDER,
    MERCENARIES_MOD_CONTROL_SLIDER,
    MERCENARIES_MOD_CONTROL_SLIDER,
    MERCENARIES_MOD_CONTROL_SLIDER,
};

static const unsigned setting_defaults[SETTING_COUNT] = {
    1u, 2u, 1u, 2u, 2u, 1u, 0u,
};

static const char *const setting_descriptions[SETTING_COUNT] = {
    "Turns the whole nuke on and off. OFF keeps every bomb vanilla.",
    "STRIKE = 25 s cloud and 700 m blast. TSAR = 30 s and 1000 m. MEGA = 40 s, 1000 m and the tallest cap.",
    "The low orange fire wave sweeping outward through the streets.",
    "EARTHQUAKE shake after every nuke: STRONG 10 s or EXTREME 20 s.",
    "NUKE ONLY replaces the vanilla blast sound with the strike cue; LAYERED keeps both.",
    "The blinding white flash over the screen when the bomb lands.",
    "Verbose log lines for tuning.",
};

/* ---------------------------------------------------------------- descriptor */

static const MercenariesMod descriptor = {
    MERCENARIES_MOD_API_VERSION,
    sizeof(MercenariesMod),
    MOD_NAME,
    MOD_VERSION,
    settings,
    SETTING_COUNT,
    world_tick,
    NULL,
    brush_end,
    NULL,
    shutdown_hook,
    "NUKE BUILT ON SVERED'S MERCFX",
    "Rebuilds the bunker buster, fuel-air bomb and cruise missile as the "
    "Mercenaries 2 World in Flames cutscene nuke: whiteout flash, orange "
    "fireball dome, red-brown mushroom cap over an orange sky, fiery stem, "
    "terrain fire wave, dust blanket, shockwaves, quake and layered audio. "
    "Needs Native Effects.",
    setting_descriptions,
    setting_controls,
    NULL,
    NULL,
    setting_defaults,
};

/* --------------------------------------------------------------- entry point */

__declspec(dllexport) const MercenariesMod *mercenaries_mod_entry(const MercenariesModHost *table)
{
    static const uint32_t flash_pixels[16] = {
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    };
    unsigned v;

    if (!table || table->api_version != MERCENARIES_MOD_API_VERSION ||
        table->size < sizeof(MercenariesModHost))
        return NULL;
    host = table;

    cfg_master = host->read_setting(MOD_INI, MOD_SECTION, "Master", cfg_master) & 1u;
    v = host->read_setting(MOD_INI, MOD_SECTION, "Yield", cfg_yield);
    if (v < 1u) v = 1u;
    if (v > 3u) v = 3u;
    cfg_yield = v;
    cfg_wave = host->read_setting(MOD_INI, MOD_SECTION, "FireWave", cfg_wave) & 1u;
    v = host->read_setting(MOD_INI, MOD_SECTION, "Earthquake", cfg_quake);
    if (v > 2u) v = 2u;
    cfg_quake = v;
    v = host->read_setting(MOD_INI, MOD_SECTION, "NukeAudio", cfg_audio);
    if (v > 2u) v = 2u;
    cfg_audio = v;
    cfg_whiteout = host->read_setting(MOD_INI, MOD_SECTION, "Whiteout", cfg_whiteout) & 1u;
    cfg_debug = host->read_setting(MOD_INI, MOD_SECTION, "DebugLog", cfg_debug) & 1u;
    v = host->read_setting(MOD_INI, MOD_SECTION, "ViewRange", (unsigned)view_range);
    if (v < 128u) v = 128u;
    if (v > 4096u) v = 4096u;
    view_range = (float)v;

    flash_texture = host->register_texture(4, 4, flash_pixels);
    have_begin = host->guest_function_exists(SPRITE_BEGIN_FN) ? 1 : 0;
    have_quad = host->guest_function_exists(SPRITE_QUAD_FN) ? 1 : 0;
    LOG("sprite helpers: begin=%d quad=%d", have_begin, have_quad);

    attach_state = 1;
    return &descriptor;
}

/* Local-build shims: no CRT startup object is linked. */
const float _fltused = 1.0f;
BOOL WINAPI _DllMainCRTStartup(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
