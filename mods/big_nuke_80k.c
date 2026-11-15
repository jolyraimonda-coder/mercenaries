/* BIG NUKE 80K v1.0.0 - a code mod for Mercenaries Recompiled 1.1.1
 * (Mod Manager / Mod API v2 + Native Effects / MercFx).
 *
 * Recreates the Arma 1 "Survive by Big Nuke 80K" nuclear bomb on the bunker
 * buster, fuel-air bomb and cruise missile strikes:
 *  - blinding whiteout flash and white-hot glowing core;
 *  - heavy ground fire wall and a burning fire pillar at ground zero;
 *  - brown-grey charcoal mushroom cap over a ropey segmented stem with a
 *    ground dust skirt;
 *  - the signature WHITE CONDENSATION RINGS expanding above the cap, three
 *    arms 120 degrees apart, rotating as they spread and fade;
 *  - grey air smoke covering the sky late in the show;
 *  - shockwave rings, ground scorch, earthquake shake, slow motion, layered
 *    nuke audio (a_strike.bb_explosion) with the default blast sound
 *    suppressible.
 *  - NO retail Meltdown doom sphere: this mod never calls the meltdown
 *    effect, the cloud and blast are entirely MercFx.
 *
 * Built on the verified MercFx calling pattern: particles are registered once
 * at attach (sprites at most 100 units, alpha at most 128, as the framework
 * requires), prepared lazily inside the impact dispatch, emitted by a
 * registered simulation timeline, and reset by the framework's world-reset
 * notification. Without Native Effects every bomb keeps its vanilla blast.
 *
 * Build (64-bit Windows DLL), with mercenaries_mod.h and mercfx.h beside this file:
 *   cl /nologo /W4 /O2 /MT /LD big_nuke_80k.c /I . /Fe:big_nuke_80k.dll /link /MACHINE:X64 kernel32.lib libucrt.lib libvcruntime.lib
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

#define MOD_NAME            "BIG NUKE 80K"
#define MOD_VERSION         "1.9.0"
#define MOD_INI             "mods\\big_nuke_80k.ini"
#define MOD_SECTION         "BigNuke80K"
#define MOD_OWNER           "big_nuke_80k"

#define EFFECT_NAME         "big_nuke_80k.main"

/* The shop strike: the vanilla bunker buster delivery, which this mod fully
   intercepts at impact - a doom-free way to call the nuke. The meltdown
   warhead air-bursts by engine script and cannot be intercepted mid-air. */
#define SHOP_TEMPLATE       "template_support_p_bunkerbuster"
#define SHOP_TEXTURE        "shop_bunkerbuster"
#define SHOP_NAME_KEY       "support.airstrikes.bignuke80k"
#define SHOP_SHORT_KEY      "shop.bignuke80k.short"
#define SHOP_LONG_KEY       "shop.bignuke80k.long"
#define SHOP_PRICE          90000u
/* Retail shop addresses (Mod Manager documentation). */
#define SHOP_OBJECT         0x0034ABC8u
#define SHOP_STOCKED_FN     0x000C8BF0u
#define SHOP_STOCK_FN       0x000CDA60u

/* Retail sprite helpers for the whiteout flash (verified addresses). */
#define SPRITE_BEGIN_FN     0x0020A0D0u
#define SPRITE_QUAD_FN      0x0020C980u
#define EXPLOSION_FN        0x000B6460u   /* retail RsExplosion detonation */

/* The game's cached textures, as the MercFx framework reads them. */
#define TEX_FLASH_HASH      0x88e27c45u
#define TEX_EXPLOSION_HASH  0x02bb1fe0u
#define TEX_SMOKE_HASH      0xc272d7afu

#define BNK_TIMELINES       2u
#define BNK_FLASH_PASSES    8u    /* stacked screen-filling flashes */
#define MAX_BRUSHES         96u
#define TAU                 6.2831853f

/* Trigger templates: every heavy strike becomes the nuke, including the
 * meltdown bomb (which the shop's frog support template delivers); our
 * configured blast replaces its vanilla doom at impact. */
static const char *const triggers[] = {
    "template_amm_bunkerbuster",
    "template_amm_bunkerbuster0",
    "template_amm_bunkerbusterbomb",
    "template_amm_fuelairbomb",
    "template_amm_fuelairbomb0",
    "template_amm_cruisemissile",
    "template_amm_cruisemissile0",
    "template_amm_meltdown",
};
#define TRIGGER_COUNT (sizeof(triggers) / sizeof(triggers[0]))
#define HASH_SLOTS 16u

/* Particle types. */
enum { P_FLASH, P_FIRE, P_RING, P_CAP, P_PILLAR, P_SMOKE, P_STEM, P_SKIRT, P_GROUND, P_COUNT };

typedef struct Timeline {
    int active;
    int mega;              /* MEGA yield: taller cap, longer show */
    float pos[3];
    float base_y;          /* ground height for the wave, skirt and marker */
    float age, duration;
    float wave_radius;     /* per-tier cap of the rotating fire wave */
    float next_wave, next_rings, next_cap, next_pillar, next_stem, next_skirt,
          next_air, next_ring, next_flash, next_marker, next_gfire, next_fireball;
    float fuse;            /* seconds of silence before the nuke detonates */
    int burst_done;
    unsigned seed;
    uint32_t shock;        /* shockwave cue handle, plays once */
} Timeline;

/* ------------------------------------------------------------------- state */

static const MercenariesModHost *host;
static const MercFxApi *fx;

static unsigned cfg_master   = 1;  /* MASTER */
static unsigned cfg_yield    = 2;  /* 1 STRIKE, 2 TSAR, 3 MEGA */
static unsigned cfg_wave     = 1;  /* DUST WAVE */
static unsigned cfg_rings    = 1;  /* COND. RINGS */
static unsigned cfg_quake    = 1;  /* 0 off, 1 strong, 2 extreme */
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

static Timeline timelines[BNK_TIMELINES];
static unsigned timeline_cursor;
static uint32_t particles[P_COUNT];

static uint32_t flash_texture;
static float flash_time = -1.0f;
/* the deferred warhead: armed at impact, detonated at fuse end in world_tick */
static float deton_pos[3], deton_radius, deton_force, deton_damage;
static int deton_pending, deton_go, explosion_ok;
static float flash_hold, flash_fade;
static unsigned brushes_this_frame, brushes_last_frame;

static int have_begin, have_quad;

static double world_time;
static int stock_checked, have_stocked, have_stock;
static double next_stock_time;
static unsigned stock_attempts;
static int logged_meltdown_advisory;

static void try_stock_item(void);   /* forward: defined below, called by world_tick */

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

#define LOG(...) do { if (host && host->log) host->log("big_nuke_80k", __VA_ARGS__); } while (0)
#define DLOG(...) do { if (cfg_debug && host && host->log) host->log("bnk_debug", __VA_ARGS__); } while (0)

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

    /* FLASH: blinding white core of the fireball. */
    s.texture_hash = TEX_FLASH_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x80FFFFFFu; s.colors[1] = 0x60FFF8E0u; s.colors[2] = 0x00FFE0A0u;
    s.widths[0] = 30.0f;  s.heights[0] = 30.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 80.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.2f;
    s.y_acceleration = 2.0f; s.friction = 0.5f;
    s.lifetime = 0.8f; s.lifetime_random = 0.3f;
    register_type(P_FLASH, &s);

    /* FIRE: orange flame (the ground fire wall and the pillar). */
    s.texture_hash = TEX_EXPLOSION_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x80FFC040u; s.colors[1] = 0x70FF7000u; s.colors[2] = 0x30A02800u;
    s.widths[0] = 25.0f;  s.heights[0] = 25.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 60.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 4.0f; s.friction = 0.3f;
    s.lifetime = 4.0f; s.lifetime_random = 1.0f;
    s.random_scale = 0.4f;
    register_type(P_FIRE, &s);

    /* RING: grey expanding ring (no more white). */
    s.texture_hash = TEX_FLASH_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x40B8B6B2u; s.colors[1] = 0x28A0A09Cu; s.colors[2] = 0x00888682u;
    s.widths[0] = 30.0f;  s.heights[0] = 20.0f;
    s.widths[1] = 70.0f;  s.heights[1] = 45.0f;
    s.widths[2] = 100.0f; s.heights[2] = 65.0f;
    s.time_b = 0.35f;
    s.y_acceleration = 0.5f; s.friction = 0.25f;
    s.lifetime = 4.0f; s.lifetime_random = 1.0f;
    register_type(P_RING, &s);

    /* CAP: warm sunlit grey head (the capture2 photo): warm grey inside,
       pale warm edges. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x68686258u; s.colors[1] = 0x50908A7Eu; s.colors[2] = 0x00C8BAA6u;
    s.widths[0] = 40.0f;  s.heights[0] = 35.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 60.0f;
    s.widths[2] = 100.0f; s.heights[2] = 85.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 2.2f; s.friction = 0.2f;
    s.lifetime = 10.0f; s.lifetime_random = 2.5f;
    s.random_scale = 0.35f; s.random_acceleration = 1.5f;
    s.wind = 1.0f;
    register_type(P_CAP, &s);

    /* PILLAR: dense grey smoke column standing at ground zero. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x68545050u; s.colors[1] = 0x50686662u; s.colors[2] = 0x00989692u;
    s.widths[0] = 30.0f;  s.heights[0] = 50.0f;
    s.widths[1] = 70.0f;  s.heights[1] = 90.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 8.0f; s.friction = 0.2f;
    s.lifetime = 6.0f; s.lifetime_random = 2.0f;
    s.random_scale = 0.3f;
    register_type(P_PILLAR, &s);

    /* GROUND: small burning flames that sweep the ground in all directions. */
    s.texture_hash = TEX_EXPLOSION_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x70FFD060u; s.colors[1] = 0x60FF7818u; s.colors[2] = 0x20802000u;
    s.widths[0] = 8.0f;   s.heights[0] = 10.0f;
    s.widths[1] = 16.0f;  s.heights[1] = 20.0f;
    s.widths[2] = 26.0f;  s.heights[2] = 32.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 3.0f; s.friction = 0.4f;
    s.lifetime = 3.0f; s.lifetime_random = 1.0f;
    s.random_scale = 0.4f;
    s.wind = 1.5f;
    register_type(P_GROUND, &s);

    /* SMOKE: warm grey air dust covering half the city, pushed by wind. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x4878746Cu; s.colors[1] = 0x30A8A094u; s.colors[2] = 0x00D8CFC0u;
    s.widths[0] = 60.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 90.0f;  s.heights[1] = 70.0f;
    s.widths[2] = 100.0f; s.heights[2] = 95.0f;
    s.time_b = 0.4f;
    s.y_acceleration = 0.5f; s.friction = 0.15f;
    s.lifetime = 20.0f; s.lifetime_random = 5.0f;
    s.wind = 3.0f;
    register_type(P_SMOKE, &s);

    /* STEM: thick grey segmented column smoke. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x68504E4Au; s.colors[1] = 0x4864625Eu; s.colors[2] = 0x0094928Eu;
    s.widths[0] = 25.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 80.0f;
    s.widths[2] = 95.0f;  s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 2.0f; s.friction = 0.25f;
    s.lifetime = 12.0f; s.lifetime_random = 3.0f;
    register_type(P_STEM, &s);

    /* SKIRT: base dust skirt around the stem foot. */
    s.texture_hash = TEX_SMOKE_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x50584E44u; s.colors[1] = 0x404C463Eu; s.colors[2] = 0x00201C18u;
    s.widths[0] = 40.0f;  s.heights[0] = 12.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 20.0f;
    s.widths[2] = 100.0f; s.heights[2] = 32.0f;
    s.time_b = 0.35f;
    s.y_acceleration = 1.0f; s.friction = 0.3f;
    s.lifetime = 15.0f; s.lifetime_random = 4.0f;
    s.wind = 2.5f;
    register_type(P_SKIRT, &s);
}

static void emit_alpha(unsigned type, const float p[3], float vx, float vy, float vz,
                       float alpha, unsigned *seed);

static void emit(unsigned type, const float p[3], float vx, float vy, float vz, unsigned *seed)
{
    emit_alpha(type, p, vx, vy, vz, 1.0f, seed);
}

static void emit_alpha(unsigned type, const float p[3], float vx, float vy, float vz,
                       float alpha, unsigned *seed)
{
    float velocity[3];
    float spin;
    if (!particles[type] || !fx || !fx->emit_particle) return;
    velocity[0] = vx; velocity[1] = vy; velocity[2] = vz;
    spin = (rand_frac(seed) - 0.5f) * 2.0f;
    fx->emit_particle(particles[type], p, velocity, spin, alpha);
}

/* ---------------------------------------------------------------- timeline */

static Timeline *grab_timeline(void)
{
    unsigned i;
    Timeline *candidate;
    for (i = 0; i < BNK_TIMELINES; ++i) {
        candidate = &timelines[timeline_cursor];
        timeline_cursor = (timeline_cursor + 1u) % BNK_TIMELINES;
        if (!candidate->active) return candidate;
    }
    return &timelines[0];   /* all busy: reuse the oldest */
}

#define FIREBALL_END        10.0f   /* the hydrogen fireball stage length */
#define MUSHROOM_START       8.0f   /* the mushroom rises after the fireball */

static float cap_height(const Timeline *t) { return t->mega ? 120.0f : 100.0f; }
static float cap_radius(const Timeline *t) { return t->mega ? 100.0f : 80.0f; }
/* the whole mushroom keeps rising as it ages (the WiC climb) */
static float cap_rise(const Timeline *t)
{
    float rise = t->age * 1.6f;
    return rise > 80.0f ? 80.0f : rise;
}

/* Heavy ground fire wall: expands while rotating around the whole area, and
   stops at the yield's blast radius (the video's burning ground). */
static void emit_wave(Timeline *t, float dt)
{
    const float spin = t->age * 2.0f;
    float radius;
    unsigned j;

    if (!cfg_wave) return;
    radius = 30.0f + t->age * 80.0f;
    if (radius >= t->wave_radius) return;
    t->next_wave -= dt;
    if (t->next_wave > 0.0f) return;
    t->next_wave = 0.15f;

    for (j = 0; j < 22; ++j) {
        float angle = spin + (TAU * (float)j) / 22.0f + rand_frac(&t->seed) * 0.15f;
        float jitter = radius + (rand_frac(&t->seed) - 0.5f) * 30.0f;
        float x = t->pos[0] + cosf(angle) * jitter;
        float z = t->pos[2] + sinf(angle) * jitter;
        float terrain = t->base_y;
        float p[3];
        if (fx && fx->sample_terrain_height) {
            float h;
            if (fx->sample_terrain_height(x, z, &h)) terrain = h;
        }
        p[0] = x; p[2] = z;
        /* grey dust front sweeping outward, fading away as it spreads */
        p[1] = terrain + 5.0f;
        emit(P_SMOKE, p, cosf(angle) * 16.0f, 2.0f, sinf(angle) * 16.0f, &t->seed);
        p[1] = terrain + 12.0f;
        emit(P_CAP, p, cosf(angle) * 9.0f, 1.5f, sinf(angle) * 9.0f, &t->seed);
    }
}

/* Stage one: the hydrogen fireball - a big orange burn with flames
   rolling outward all around, before the mushroom rises. */
static void emit_fireball(Timeline *t, float dt)
{
    const float fade = t->age > FIREBALL_END - 2.0f
        ? 1.0f - (t->age - (FIREBALL_END - 2.0f)) / 2.0f * 0.7f
        : 1.0f;
    unsigned j;

    t->next_fireball -= dt;
    if (t->next_fireball > 0.0f) return;
    t->next_fireball = 0.12f;
    if (t->age > FIREBALL_END) return;

    /* the burning core: big orange columns climbing at ground zero */
    for (j = 0; j < 3; ++j) {
        float p[3];
        p[0] = t->pos[0] + (rand_frac(&t->seed) - 0.5f) * 30.0f;
        p[2] = t->pos[2] + (rand_frac(&t->seed) - 0.5f) * 30.0f;
        p[1] = t->base_y + 4.0f + 50.0f * rand_frac(&t->seed) + t->age * 3.0f;
        emit_alpha(P_FIRE, p, 0.0f, 9.0f + 4.0f * rand_frac(&t->seed), 0.0f, fade, &t->seed);
    }
    /* flames rolling outward in every direction */
    for (j = 0; j < 3; ++j) {
        float angle = TAU * rand_frac(&t->seed) + t->age * 0.8f;
        float radius = 8.0f + t->age * 30.0f;
        float terrain = t->base_y;
        float p[3];
        if (fx && fx->sample_terrain_height) {
            float h;
            if (fx->sample_terrain_height(t->pos[0] + cosf(angle) * radius,
                                          t->pos[2] + sinf(angle) * radius, &h))
                terrain = h;
        }
        p[0] = t->pos[0] + cosf(angle) * radius;
        p[2] = t->pos[2] + sinf(angle) * radius;
        p[1] = terrain + 2.0f;
        emit_alpha(P_FIRE, p, cosf(angle) * 10.0f, 4.0f, sinf(angle) * 10.0f, fade, &t->seed);
    }
}

/* Late-show fade: emissions thin out and turn translucent so the whole
   cloud dissolves gradually instead of ending abruptly. */
static float cloud_fade(const Timeline *t)
{
    float f = 1.0f - (t->age - t->duration * 0.6f) / (t->duration * 0.4f) * 0.6f;
    return f < 0.4f ? 0.4f : f;
}

/* Grey dust column rising from ground zero. */
static void emit_pillar(Timeline *t, float dt)
{
    unsigned j;

    t->next_pillar -= dt;
    if (t->next_pillar > 0.0f) return;
    t->next_pillar = 0.12f;
    if (t->age < MUSHROOM_START || t->age > 22.0f) return;

    for (j = 0; j < 2; ++j) {
        float p[3];
        p[0] = t->pos[0] + (rand_frac(&t->seed) - 0.5f) * 18.0f;
        p[2] = t->pos[2] + (rand_frac(&t->seed) - 0.5f) * 18.0f;
        p[1] = t->base_y + 2.0f + 40.0f * rand_frac(&t->seed);
        emit_alpha(P_CAP, p, 0.0f, 5.0f, 0.0f, cloud_fade(t), &t->seed);
    }
}

/* The wide flat ring riding around the cap, rotating as it spreads
   (the capture4 donut). Grey-white, dense, slowly turning. */
static void emit_rings(Timeline *t, float dt)
{
    const float spin = t->age * 0.9f;
    if (t->age < MUSHROOM_START) return;   /* the ring rides the mushroom only */
    const float deck = cap_height(t) + cap_rise(t);
    const float radius = cap_radius(t) * 1.5f + t->age * 1.5f;
    const float fade = cloud_fade(t);
    unsigned j, arm, per = (t->age > t->duration * 0.7f) ? 2u : 3u;   /* thinner late */

    t->next_rings -= dt;
    if (t->next_rings > 0.0f) return;
    t->next_rings = 0.15f;
    if (t->age < 3.0f || t->age > t->duration * 0.9f) return;

    for (arm = 0; arm < 3; ++arm) {
        float base = spin + (TAU * (float)arm) / 3.0f;
        for (j = 0; j < per; ++j) {
            float angle = base + (TAU * (float)j) / 3.0f + rand_frac(&t->seed) * 0.2f;
            float jitter = radius + (rand_frac(&t->seed) - 0.5f) * 30.0f;
            float p[3];
            p[0] = t->pos[0] + cosf(angle) * jitter;
            p[2] = t->pos[2] + sinf(angle) * jitter;
            p[1] = deck - 10.0f + (rand_frac(&t->seed) - 0.5f) * 10.0f;
            emit_alpha(P_RING, p, cosf(angle) * 6.0f, 0.5f, sinf(angle) * 6.0f,
                       fade, &t->seed);
        }
    }
}

/* Brown-grey charcoal cap: rotating dome, early fire glow inside. */
/* Small burning flames sweeping the ground in every direction, slowly
   rotating; while the show fades away they rise higher and burn smaller. */
static void emit_ground_fire(Timeline *t, float dt)
{
    const float late = t->duration * 0.55f;
    unsigned j;

    t->next_gfire -= dt;
    if (t->next_gfire > 0.0f) return;
    t->next_gfire = 0.18f;
    if (t->age > t->duration * 0.85f) return;

    for (j = 0; j < 3; ++j) {
        float angle = TAU * rand_frac(&t->seed) + t->age * 1.3f;   /* the rotation */
        float radius = (18.0f + t->age * 50.0f) * (0.6f + 0.4f * rand_frac(&t->seed));
        if (radius > t->wave_radius * 0.75f) radius = t->wave_radius * 0.75f;
        float x = t->pos[0] + cosf(angle) * radius;
        float z = t->pos[2] + sinf(angle) * radius;
        float terrain = t->base_y;
        float p[3];
        if (fx && fx->sample_terrain_height) {
            float h;
            if (fx->sample_terrain_height(x, z, &h)) terrain = h;
        }
        p[0] = x; p[2] = z;
        p[1] = terrain + 2.0f;
        if (t->age > late) p[1] += (t->age - late) * 4.0f;   /* the fade: flames climb */
        emit(P_GROUND, p, cosf(angle) * 9.0f, 1.5f, sinf(angle) * 9.0f, &t->seed);
    }
}

static void emit_cap(Timeline *t, float dt)
{
    const float deck = cap_height(t) + cap_rise(t) + sinf(t->age * 0.9f) * 5.0f;
    if (t->age < MUSHROOM_START) return;   /* the fireball plays first */
    const float unfurl = t->age < 12.0f ? t->age / 12.0f : 1.0f;
    const float spin = t->age * 0.7f;
    unsigned j;

    const float fade = cloud_fade(t);
    unsigned n = (t->age > t->duration * 0.75f) ? 12u : 18u;   /* density drops late */

    t->next_cap -= dt;
    if (t->next_cap > 0.0f) return;
    t->next_cap = 0.12f;
    if (t->age > t->duration * 0.85f) return;   /* emission ends; the rest drifts away */

    for (j = 0; j < n; ++j) {
        float angle = spin + TAU * rand_frac(&t->seed);
        float radius = cap_radius(t) * unfurl * sqrtf(rand_frac(&t->seed));
        float p[3];
        p[0] = t->pos[0] + cosf(angle) * radius;
        p[2] = t->pos[2] + sinf(angle) * radius;
        if (j < 12) {
            /* grey head */
            p[1] = t->pos[1] + deck - 8.0f + 18.0f * rand_frac(&t->seed);
            if (t->age > t->duration * 0.6f && rand_frac(&t->seed) < 0.4f)
                /* the fade: grey mixed with warm white together */
                emit_alpha(P_SMOKE, p, cosf(angle) * 2.5f, (t->age < 6.0f) ? 7.0f : 1.0f,
                           sinf(angle) * 2.5f, fade, &t->seed);
            else
                emit_alpha(P_CAP, p, cosf(angle) * 2.5f, (t->age < 6.0f) ? 7.0f : 1.0f,
                           sinf(angle) * 2.5f, fade, &t->seed);
        } else if (j < 18) {
            /* grey smoke in the gaps and along the rim */
            p[1] = t->pos[1] + deck - 5.0f + 6.0f * rand_frac(&t->seed);
            emit_alpha(P_SMOKE, p, cosf(angle) * 2.5f, 0.9f, sinf(angle) * 2.5f,
                       fade, &t->seed);
        } else {
            /* wide dark skirt under the head */
            radius = cap_radius(t) * 1.35f * unfurl * sqrtf(rand_frac(&t->seed));
            p[0] = t->pos[0] + cosf(angle) * radius;
            p[2] = t->pos[2] + sinf(angle) * radius;
            p[1] = t->pos[1] + deck - 12.0f + 4.0f * rand_frac(&t->seed);
            emit_alpha(P_CAP, p, cosf(angle) * 2.0f, 0.7f, sinf(angle) * 2.0f,
                       fade, &t->seed);
        }
    }
}

/* Ropey segmented stem: puffs stacked in discrete lumps. */
static void emit_stem(Timeline *t, float dt)
{
    if (t->age < MUSHROOM_START) return;   /* the column follows the cap */
    unsigned j;

    t->next_stem -= dt;
    if (t->next_stem > 0.0f) return;
    t->next_stem = 0.15f;

    for (j = 0; j < 4; ++j) {
        /* ropey lumps running the whole way up, ground to the rising cap */
        float span = (t->pos[1] - t->base_y) + cap_height(t) + cap_rise(t) * 0.8f;
        float lump = ((float)((unsigned)(rand_frac(&t->seed) * 5.0f)) / 5.0f) * span;
        float angle = TAU * rand_frac(&t->seed);
        float radius = 6.0f + 9.0f * rand_frac(&t->seed);
        float p[3];
        p[0] = t->pos[0] + cosf(angle) * radius;
        p[1] = t->base_y + 4.0f + lump + (span / 10.0f) * rand_frac(&t->seed);
        p[2] = t->pos[2] + sinf(angle) * radius;
        emit_alpha(P_STEM, p, cosf(angle) * 0.4f, 3.0f + 2.0f * rand_frac(&t->seed),
                   sinf(angle) * 0.4f, cloud_fade(t), &t->seed);
    }
}

/* Grey air smoke covering the sky late in the show. */
static void emit_air(Timeline *t, float dt)
{
    unsigned j;

    t->next_air -= dt;
    if (t->next_air > 0.0f) return;
    t->next_air = 0.45f;
    if (t->age < 12.0f || t->age > 30.0f) return;   /* late emission stops; the old haze fades away */

    for (j = 0; j < 3; ++j) {
        float angle = TAU * rand_frac(&t->seed);
        float radius = (40.0f + t->age * 25.0f) * sqrtf(rand_frac(&t->seed));
        float p[3];
        p[0] = t->pos[0] + cosf(angle) * radius;
        p[2] = t->pos[2] + sinf(angle) * radius;
        p[1] = t->pos[1] + cap_height(t) * (0.4f + 1.6f * rand_frac(&t->seed));
        emit(P_SMOKE, p, cosf(angle) * 5.0f, 0.8f, sinf(angle) * 5.0f, &t->seed);
    }
}

/* Ground dust skirt around the stem foot. */
static void emit_skirt(Timeline *t, float dt)
{
    const float cap = t->wave_radius * 1.3f;   /* the dust covers almost half the city */
    unsigned j;

    t->next_skirt -= dt;
    if (t->next_skirt > 0.0f) return;
    t->next_skirt = 0.34f;
    if (t->age > 30.0f) return;   /* late emission stops; the old dust fades away */

    for (j = 0; j < 3; ++j) {
        float angle = TAU * rand_frac(&t->seed) + t->age * 0.6f;   /* slow 360 sweep */
        float radius = (25.0f + t->age * 48.0f) * sqrtf(rand_frac(&t->seed));
        if (radius > cap) radius = cap * sqrtf(rand_frac(&t->seed));
        float x = t->pos[0] + cosf(angle) * radius;
        float z = t->pos[2] + sinf(angle) * radius;
        float terrain = t->base_y;
        float p[3];
        if (fx && fx->sample_terrain_height) {
            float h;
            if (fx->sample_terrain_height(x, z, &h)) terrain = h;
        }
        p[0] = x; p[2] = z;
        p[1] = terrain + 1.5f;
        emit(P_SKIRT, p, cosf(angle) * 8.0f, 1.5f, sinf(angle) * 8.0f, &t->seed);
    }
}

/* The detonation moment: white core, grey bloom and a ground dust ring. */
static void initial_burst(Timeline *t)
{
    unsigned i, seed = t->seed ^ 0x11ABF00Du;
    float p[3];

    for (i = 0; i < 3; ++i) {
        p[0] = t->pos[0];
        p[1] = t->pos[1] + 2.0f;
        p[2] = t->pos[2];
        emit(P_FLASH, p, 0.0f, 0.0f, 0.0f, &seed);
    }
    for (i = 0; i < 20; ++i) {
        float angle = TAU * rand_frac(&seed);
        float speed = 8.0f + 16.0f * rand_frac(&seed);
        p[0] = t->pos[0];
        p[1] = t->pos[1] + 2.0f;
        p[2] = t->pos[2];
        emit(P_CAP, p, cosf(angle) * speed, 2.0f + 2.0f * rand_frac(&seed),
             sinf(angle) * speed, &seed);
    }
    for (i = 0; i < 16; ++i) {
        float angle = TAU * (float)i / 16.0f;
        p[0] = t->pos[0] + cosf(angle) * 5.0f;
        p[1] = t->base_y + 0.75f;
        p[2] = t->pos[2] + sinf(angle) * 5.0f;
        emit(P_SMOKE, p, cosf(angle) * 26.0f, 0.6f, sinf(angle) * 26.0f, &seed);
    }
    /* the burst of grey air smoke that pushes out as the nuke goes */
    for (i = 0; i < 12; ++i) {
        float angle = TAU * rand_frac(&seed);
        p[0] = t->pos[0] + cosf(angle) * 18.0f;
        p[1] = t->pos[1] + 10.0f + 30.0f * rand_frac(&seed);
        p[2] = t->pos[2] + sinf(angle) * 18.0f;
        emit(P_SMOKE, p, cosf(angle) * 14.0f, 3.0f, sinf(angle) * 14.0f, &seed);
    }
}

static void __cdecl bnk_timeline_tick(float delta_seconds, void *userdata)
{
    unsigned i;
    (void)userdata;
    if (!cfg_master) return;
    for (i = 0; i < BNK_TIMELINES; ++i) {
        Timeline *t = &timelines[i];
        if (!t->active) continue;
        t->age += delta_seconds;

        /* the fuse: the warhead sits for five seconds, then the nuke goes */
        if (t->fuse > 0.0f) {
            t->fuse -= delta_seconds;
            if (t->fuse <= 0.0f && !t->burst_done) {
                t->burst_done = 1;
                if (deton_pending) deton_go = 1;   /* the nuke goes: world_tick detonates */
                initial_burst(t);
                if (cfg_whiteout && flash_texture) {
                    flash_hold = (cfg_yield == 3) ? 2.6f : 1.8f;
                    flash_fade = (cfg_yield == 3) ? 5.0f : 4.0f;
                    flash_time = 0.0f;
                }
                if (cfg_audio != 0 && sound_primary && fx->sound_ready &&
                    fx->sound_ready(sound_primary))
                    fx->play_sound(sound_primary, t->pos);
                if (fx->request_slowdown) fx->request_slowdown(MOD_OWNER, 0.6f, 1.0f);
            }
            if (t->fuse > 0.0f) continue;   /* nothing shows until it detonates */
        }

        /* shockwave cue once, when the wave starts rolling */
        if (t->shock && t->age >= 0.9f) {
            if (fx->sound_ready && fx->sound_ready(t->shock))
                fx->play_sound(t->shock, t->pos);
            t->shock = 0;
        }

        /* the ring where the nuke opens mid-air: a white circle expanding
           around the burst point during the first 3 s */
        t->next_marker -= delta_seconds;
        if (t->next_marker <= 0.0f && t->age < 3.0f) {
            float p[3];
            unsigned k;
            t->next_marker = 0.10f;
            for (k = 0; k < 8; ++k) {
                float angle = (TAU * (float)k) / 8.0f + t->age * 1.5f;
                float radius = 8.0f + t->age * 30.0f;
                p[0] = t->pos[0] + cosf(angle) * radius;
                p[2] = t->pos[2] + sinf(angle) * radius;
                p[1] = t->pos[1] + (rand_frac(&t->seed) - 0.5f) * 6.0f;
                emit(P_RING, p, cosf(angle) * 10.0f, 0.5f, sinf(angle) * 10.0f, &t->seed);
            }
        }

        /* white-hot core flash at the detonation point for the first 4 s */
        t->next_flash -= delta_seconds;
        if (t->next_flash <= 0.0f && t->age < 4.0f) {
            float p[3];
            t->next_flash = 0.06f;
            p[0] = t->pos[0];
            p[2] = t->pos[2];
            p[1] = t->pos[1] + 6.0f + 16.0f * rand_frac(&t->seed);
            emit(P_FLASH, p, 0.0f, 0.0f, 0.0f, &t->seed);
        }

        emit_wave(t, delta_seconds);
        emit_pillar(t, delta_seconds);
        emit_rings(t, delta_seconds);
        emit_cap(t, delta_seconds);
        emit_stem(t, delta_seconds);
        emit_air(t, delta_seconds);
        emit_skirt(t, delta_seconds);

        if (t->age >= t->duration) t->active = 0;
    }
}

static void __cdecl bnk_timeline_reset(void *userdata)
{
    deton_pending = 0; deton_go = 0;
    (void)userdata;
    (void)userdata;
    memset(timelines, 0, sizeof(timelines));
    flash_time = -1.0f;
}

/* ---------------------------------------------------------------- attach */

static MercFxDecision __cdecl wic_impact(const MercFxImpact *event, void *userdata);

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
            LOG("native_effects.dll not loaded; WORLD IN CONFLICT stays vanilla");
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
        !fx->request_slowdown || !fx->register_simulation ||
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

    if (!fx->register_effect(EFFECT_NAME, wic_impact, NULL)) {
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

    if (!fx->register_simulation(bnk_timeline_tick, bnk_timeline_reset, NULL))
        LOG("timeline registration failed; the cloud will not build");

    LOG("attached: bound=%u of %u, master=%u, yield=%u, quake=%u, audio=%u, whiteout=%u",
        bound_count, (unsigned)TRIGGER_COUNT, cfg_master, cfg_yield, cfg_quake,
        cfg_audio, cfg_whiteout);
    if (!logged_meltdown_advisory) {
        logged_meltdown_advisory = 1;
        LOG("the meltdown warhead opens mid-air by engine script; order the "
            "BIG NUKE 80K airstrike from the shop for a doom-free nuke");
    }
    return 1;
}

/* ------------------------------------------------------------ impact handler */

static void quake_opts(MercFxBlastOptions *options)
{
    if (cfg_quake == 1) { options->shake_duration = 10.0f; options->shake_magnitude = 3.0f; }
    else if (cfg_quake == 2) { options->shake_duration = 18.0f; options->shake_magnitude = 4.0f; }
}

static MercFxDecision __cdecl wic_impact(const MercFxImpact *event, void *userdata)
{
    MercFxImpact *ev = (MercFxImpact *)event;   /* framework-owned copy */
    MercFxBlastOptions options;
    Timeline *t;
    unsigned i;
    int known = 0;

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

    ev->splash_damage *= 8.0f;
    ev->splash_force *= 8.0f;
    ev->splash_radius = (cfg_yield == 1) ? 600.0f : (cfg_yield == 3) ? 1000.0f : 800.0f;

    if (explosion_ok) {
        /* the warhead sits quiet for five seconds; the buildings fall when
           the mushroom arrives, not on impact */
        deton_pos[0] = event->position[0];
        deton_pos[1] = event->position[1];
        deton_pos[2] = event->position[2];
        deton_radius = ev->splash_radius;
        deton_force = ev->splash_force;
        deton_damage = ev->splash_damage;
        deton_pending = 1;
        deton_go = 0;
    } else {
        /* no engine access: fall back to the instant framework blast */
        memset(&options, 0, sizeof(options));
        options.size = sizeof(options);
        options.damage_type = MERCFX_BLAST_BUNKERBUSTER;
        options.scorch_radius = 40.0f;
        quake_opts(&options);
        if (cfg_audio == 2 && sound_primary && fx->sound_ready && fx->sound_ready(sound_primary))
            options.suppress_default_sound = 1u;      /* NUKE ONLY */
        if (!fx->apply_blast_configured(event, &options)) {
            LOG("blast rejected by the framework; the vanilla explosion took over");
            return MERCFX_UNHANDLED;
        }
    }


    /* start the sustained cloud timeline */
    t = grab_timeline();
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->mega = (cfg_yield == 3);
    t->age = 0.0f;
    t->duration = (cfg_yield == 1) ? 30.0f : (cfg_yield == 3) ? 60.0f : 45.0f;
    t->wave_radius = (cfg_yield == 1) ? 550.0f : (cfg_yield == 3) ? 950.0f : 750.0f;
    t->pos[0] = event->position[0];
    t->pos[1] = event->position[1] + 45.0f;   /* the nuke opens high mid-air */
    t->pos[2] = event->position[2];
    t->base_y = event->position[1];
    t->fuse = 5.0f;                           /* five seconds of silence, then it goes */
    t->burst_done = 0;
    t->next_marker = 0.0f;
    t->next_wave = 0.02f;
    t->next_rings = 0.05f;
    t->next_cap = 0.0f;
    t->next_pillar = 0.10f;
    t->next_stem = 0.05f;
    t->next_skirt = 0.30f;
    t->next_gfire = 0.15f;
    t->next_fireball = 0.0f;
    t->next_air = 0.20f;
    t->next_ring = 2.5f;
    t->next_flash = 0.0f;
    t->seed = 0x33121c47u ^ (unsigned)(int)finite3(event->position);
    t->shock = (cfg_audio == 1 && sound_primary) ? sound_primary : 0;

    LOG("big nuke 80k: yield=%u r=%.0f at %.0f %.0f %.0f",
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
    per_brush = 1.0f - powf(1.0f - alpha / (float)BNK_FLASH_PASSES, 1.0f / (float)n);
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
    for (pass = 0; pass < BNK_FLASH_PASSES; ++pass) {
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

    /* the mushroom has risen: now the nuke really goes off */
    if (deton_pending && deton_go && explosion_ok) {
        uint32_t args[13];
        memcpy(args, deton_pos, 12);
        args[3] = float_bits(deton_radius);   /* blast radius */
        args[4] = float_bits(deton_force);    /* force */
        args[5] = 0u;                         /* min damage */
        args[6] = float_bits(deton_damage);   /* max damage */
        args[7] = 0u;                         /* no land visual: our cloud plays */
        args[8] = 0u;                         /* our cue replaces the stock sound */
        args[9] = 3u;                         /* bunker-buster damage type */
        args[10] = 0u;                        /* no owner obstacle */
        args[11] = 1u;                        /* scaled force */
        args[12] = 0u;                        /* normal cover */
        host->guest_call(EXPLOSION_FN, 0u, 13u, args);
        deton_pending = 0;
        deton_go = 0;
        LOG("detonation: r=%.0f dmg=%.0f at %.0f %.0f %.0f",
            deton_radius, deton_damage, deton_pos[0], deton_pos[1], deton_pos[2]);
    }

    if (next_stock_time == 0.0 && world_time >= 0.5) {
        next_stock_time = world_time;
        try_stock_item();
    } else if (next_stock_time > 0.0 && world_time >= next_stock_time + 30.0 &&
               stock_attempts < 3) {
        ++stock_attempts;
        next_stock_time = world_time;
        try_stock_item();
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
        if (fx->unregister_simulation) fx->unregister_simulation(bnk_timeline_tick, NULL);
    }
    fx = NULL;
}

/* ---------------------------------------------------------------- shop item */

static const MercenariesShopItem shop_item = {
    SHOP_TEMPLATE,
    SHOP_NAME_KEY,
    SHOP_SHORT_KEY,
    SHOP_LONG_KEY,
    SHOP_TEXTURE,
    SHOP_PRICE,
    MERCENARIES_SHOP_AIR_STRIKES,
};

static void register_shop_item(void)
{
    if (!host || !host->add_shop_item) return;
    if (host->size < offsetof(MercenariesModHost, add_shop_item) + sizeof(host->add_shop_item)) return;
    if (host->register_string) {
        host->register_string(SHOP_NAME_KEY, "BIG NUKE 80K");
        host->register_string(SHOP_SHORT_KEY, "The 80-kiloton big nuke.");
        host->register_string(SHOP_LONG_KEY,
            "One bomber, one 80-kiloton warhead. Whiteout, rotating fire wall, "
            "grey mushroom and condensation rings. Leave nothing standing.");
    }
    if (host->add_shop_item(&shop_item))
        LOG("shop item BIG NUKE 80K added on %s", SHOP_TEMPLATE);
    else
        LOG("shop item refused (already sold, or the Mod Manager is too old)");
}

static void try_stock_item(void)
{
    uint32_t hash = fnv1a_casefold(SHOP_TEMPLATE);
    const uint32_t check_args[1] = {hash};
    const uint32_t stock_args[2] = {hash, 0u};

    if (!stock_checked) {
        have_stocked = host->guest_function_exists(SHOP_STOCKED_FN) ? 1 : 0;
        have_stock = host->guest_function_exists(SHOP_STOCK_FN) ? 1 : 0;
        stock_checked = 1;
    }
    if (!have_stock) return;
    if (have_stocked && host->guest_call(SHOP_STOCKED_FN, 0u, 1u, check_args)) return;
    host->guest_call(SHOP_STOCK_FN, SHOP_OBJECT, 2u, stock_args);
    LOG("stocked BIG NUKE 80K in the Merchant of Menace");
    next_stock_time = world_time + 30.0;
}

/* --------------------------------------------------------------- settings */

static unsigned get_master(void)   { return cfg_master; }
static unsigned get_yield(void)    { return cfg_yield; }
static unsigned get_wave(void)     { return cfg_wave; }
static unsigned get_rings(void) { return cfg_rings; }
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
SET_on("Rings", rings)
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

#define SETTING_COUNT 8u

static const MercenariesModSetting settings[SETTING_COUNT] = {
    {"MASTER",       on_off, 2, get_master,    set_master},
    {"YIELD",        yield_labels, 4, get_yield, set_yield},
    {"DUST WAVE",    on_off, 2, get_wave,      set_wave},
    {"COND. RINGS",  on_off, 2, get_rings,     set_rings},
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
    MERCENARIES_MOD_CONTROL_SLIDER,
};

static const unsigned setting_defaults[SETTING_COUNT] = {
    1u, 2u, 1u, 1u, 1u, 2u, 1u, 0u,
};

static const char *const setting_descriptions[SETTING_COUNT] = {
    "Turns the whole nuke on and off. OFF keeps every bomb vanilla.",
    "STRIKE = 30 s show and 600 m blast. TSAR = 45 s and 800 m. MEGA = 60 s, 1000 m and the tallest cap.",
    "The grey dust wave sweeping outward as the orange fades away."
    "The white condensation rings expanding above the mushroom cap.",
    "EARTHQUAKE shake after every nuke: STRONG 10 s or EXTREME 18 s.",
    "NUKE ONLY replaces the vanilla blast sound with the strike cue; LAYERED keeps both.",
    "The long blinding white flash over the screen when the bomb lands.",
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
    "The Arma Survive by Big Nuke 80K bomb on every heavy strike: whiteout "
    "flash, grey dust wave sweeping outward, grey mushroom with little flames "
    "around the head, ropey stem down to the ground, white condensation rings "
    "above the cap, and grey air smoke that fades away. No red shockwave, no "
    "Meltdown doom sphere. Needs Native Effects.",
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
    cfg_rings = host->read_setting(MOD_INI, MOD_SECTION, "Rings", cfg_rings) & 1u;
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
    register_shop_item();
    explosion_ok = host->guest_function_exists(EXPLOSION_FN) ? 1 : 0;
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
