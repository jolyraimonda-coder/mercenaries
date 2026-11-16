/* MERC 2 NUKE BUNKER BUSTER v1.2.0 - a code mod for Mercenaries Recompiled 1.1.1
 * (Mod Manager / Mod API v2 + Native Effects / MercFx).
 *
 * Written fresh from the MERCENARIES 2: WORLD IN FLAMES PC scripts and game
 * data. Nothing here is carried over from another nuke mod: the show below
 * is a direct transcription of the M2 source, which was mined block by block
 * out of the M2 FFCS wads (vz.wad -> sges blocks -> raw deflate -> UCFX).
 *
 * THE M2 SCRIPT THIS MOD PLAYS (AIO_SpawnMenu+OrdnanceDrops.lua,
 * Mercenaries2Remastered, function playFX):
 *
 *     if fxType == "NUKE" then
 *       fx("global_particle_airstrike_tactnuke", bx, detY, bz)      -- t0
 *       after(1.0, function()
 *         fx("global_particle_exp_shockwave_ground_tactnuke", ...)  -- t+1.0
 *       end)
 *
 *     ordnance "Tactical Nuke": proto "Nuclear Bunker Buster Projectile",
 *       height = 80, vel = -90, dist = 100   -- fall = 100/90 = 1.11 s
 *
 * The story nuke (M2 contract pmccon004, the "ChinaNuked" ending) fires the
 * same pair of effects through its loc_tactnuke + loc_shockwave markers and
 * the ChinaNuked_Particle set with a white-blue light
 * (Light_large_whiteblue_cumana). This mod rebuilds exactly that sequence:
 *
 *   t0     DETONATE  - the M2 tactnuke FX: white-blue light flash, glare
 *                      streaks, orange fireball opening at ground zero and
 *                      the grey dust dome; the warhead's damage lands here
 *   t+1.0  SHOCKWAVE - the M2 ground shockwave FX: one big flat grey dust
 *                      ring hugging the ground, racing outward, no spin
 *   t0..t40 MUSHROOM - the M2 photo cloud: the WHOLE mushroom burns for ten
 *                      seconds (huge self-lit fire cap, radius 190) over a
 *                      dark collar and a thick stem carrying fire inside,
 *                      then the smoke takes over and fades away
 *   late    DUST     - grey air smoke thinning out to the end
 *
 * NO RED SHOCKWAVE: the detonation always uses the plain explosion damage
 * type (the retail bunker-buster type draws the red decal; M2 has no red
 * shockwave and neither does this mod), no retail meltdown effect, no engine
 * shockwave call. NO YIELD SIZES: M2 has one tactical nuke, so this mod has
 * one show, tuned to the M2 look.
 *
 * The framework's MercFx particle limits are respected: sprites at most 100
 * units, registration alpha at most 128 (retail full alpha is 0x80).
 *
 * Build (64-bit Windows DLL), with mercenaries_mod.h and mercfx.h beside it:
 *   cl /nologo /W4 /O2 /MT /LD m2_bunker_nuke.c /I . /Fe:m2_bunker_nuke.dll /link /MACHINE:X64 kernel32.lib libucrt.lib libvcruntime.lib
 *
 * v1.2.0 changes: fog-package fixes merged in - registration retries, the
 * proven flash spec, ViewRange 8192, and debug logging for field diagnosis.
 * The recomp-side draw distance (DrawDistance / ObjectDrawDistance = 1)
 * ships separately in the game folder fix pack.
 */

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "mercenaries_mod.h"
#include "mercfx.h"

/* ------------------------------------------------------------------ tuning */

#define MOD_NAME            "MERC 2 NUKE BUNKER BUSTER"
#define MOD_VERSION         "1.2.0"
#define MOD_INI             "mods\\m2_bunker_nuke.ini"
#define MOD_SECTION         "M2BunkerNuke"
#define MOD_OWNER           "m2_bunker_nuke"

#define EFFECT_NAME         "m2_bunker_nuke.main"

/* The shop strike: the vanilla bunker buster delivery, which this mod fully
   intercepts at impact - the doom-free way to call the nuke. The meltdown
   template is also bound because the meltdown warhead detonates by engine
   script when it hits the ground, and the interception then replaces its
   doom visuals with this show. */
#define SHOP_TEMPLATE       "template_support_p_bunkerbuster"
#define SHOP_TEXTURE        "shop_bunkerbuster"
#define SHOP_NAME_KEY       "support.airstrikes.m2tactnuke"
#define SHOP_SHORT_KEY      "shop.m2tactnuke.short"
#define SHOP_LONG_KEY       "shop.m2tactnuke.long"
#define SHOP_PRICE          90000u

/* Retail shop addresses (Mod Manager documentation). */
#define SHOP_OBJECT         0x0034ABC8u
#define SHOP_STOCKED_FN     0x000C8BF0u
#define SHOP_STOCK_FN       0x000CDA60u

/* Retail sprite helpers for the whiteout flash (verified addresses). */
#define SPRITE_BEGIN_FN     0x0020A0D0u
#define SPRITE_QUAD_FN      0x0020C980u
#define EXPLOSION_FN        0x000B6460u   /* retail RsExplosion detonation */

/* The M2 show, one size: the story nuke of pmccon004. */
#define SHOW_DURATION       40.0f         /* seconds of cloud */
#define SPLASH_RADIUS       800.0f        /* the blast radius of the show */
#define SHOCK_RADIUS        700.0f        /* where the ring stops */
#define SHOCK_DELAY         1.0f          /* the Lua's after(1.0, ...) */
#define FIREBALL_TIME       3.5f          /* the orange fireball burns out */
#define CAP_TOP_TIME        6.0f          /* the cap is up by six seconds */
#define CAP_HEIGHT          95.0f
#define CAP_RADIUS          190.0f        /* the M2 cap is enormous, see the photo */
#define CAP_RISE_MAX        85.0f
#define FIRE_TIME           10.0f         /* the whole cloud BURNS this long */
#define FOOT_FIRE_TIME      6.0f          /* flames at the cap foot */
#define DUST_CAP            450.0f        /* the skirt's widest radius */
#define FLASH_HOLD          1.0f          /* white screen, like the M2 flash */
#define FLASH_FADE          3.0f
#define FLASH_PASSES        3u            /* stacked quads for a solid white */

/* The warhead's damage: the retail splash scaled up so the nuke clears its
   radius, matching how the M2 story nuke erases its target area. */
#define DAMAGE_SCALE        8.0f
#define FORCE_SCALE         8.0f

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
#define HASH_SLOTS TRIGGER_COUNT

/* ------------------------------------------------------------- tiny helpers */

#define TAU 6.2831853f

static const MercenariesModHost *host;
static const MercFxApi *fx;
static unsigned attach_state;          /* 0 idle, 1 waiting, 2 live, 3 vanilla */
static unsigned attach_attempts;
static float view_range = 4096.0f;

static uint32_t trigger_hash[HASH_SLOTS];
static unsigned char bound_flags[HASH_SLOTS];
static unsigned bound_count;
static int stock_checked, have_stocked, have_stock, stock_attempts;
static double world_time, next_stock_time;
static int logged_advisory;

static uint32_t particles[9];
enum { P_LIGHT, P_GLARE, P_FIREBALL, P_FIRECAP, P_SHOCK, P_CAP, P_STEM, P_DUST, P_BASE, P_COUNT_IDX };
#define P_COUNT P_COUNT_IDX

static uint32_t sound_primary;
static uint32_t flash_texture;
static int have_begin, have_quad;
static float flash_time = -1.0f;
static unsigned brushes_this_frame, brushes_last_frame;
#define MAX_BRUSHES 512u

static unsigned cfg_master = 1;    /* MASTER */
static unsigned cfg_shock = 1;     /* SHOCKWAVE, the +1.0 s grey ring */
static unsigned cfg_fire = 1;      /* FIREBALL */
static unsigned cfg_whiteout = 1;  /* WHITEOUT */
static unsigned cfg_quake = 1;     /* EARTHQUAKE 0 off, 1 strong, 2 extreme */
static unsigned cfg_audio = 2;     /* NUKE AUDIO 0 off, 1 layered, 2 nuke only */
static unsigned cfg_debug = 0;     /* DEBUG LOG */

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

static float rand_frac(unsigned *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return (float)(*seed & 0x00FFFFFFu) / 16777216.0f;
}

#define LOG(...) do { if (host && host->log) host->log("m2_bunker_nuke", __VA_ARGS__); } while (0)
#define DLOG(...) do { if (cfg_debug && host && host->log) host->log("m2_bunker_nuke_debug", __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ show */

/* One show at a time is enough: M2 has one tactical nuke. The struct holds
   the scripted sequence state plus the per-emitter clocks. */
typedef struct Show {
    unsigned char active;
    float age;                        /* seconds since detonation */
    unsigned script_next;             /* next event of the M2 script table */
    float pos[3];                     /* detonation point (ground + 2) */
    float base_y;
    unsigned seed;
    float next_light, next_fire, next_shock, next_cap, next_stem, next_dust;
} Show;

#define SHOW_COUNT 2
static Show shows[SHOW_COUNT];
static unsigned show_cursor;

static Show *grab_show(void)
{
    unsigned i;
    Show *s;
    for (i = 0; i < SHOW_COUNT; ++i) {
        s = &shows[show_cursor];
        show_cursor = (show_cursor + 1u) % SHOW_COUNT;
        if (!s->active) return s;
    }
    return &shows[0];                  /* all busy: reuse the oldest */
}

/* ------------------------------------------------------------ particles */

static void register_particle_type(unsigned index, const MercFxParticle *spec)
{
    uint32_t handle;
    if (index >= P_COUNT) return;
    handle = fx->register_particle(spec);
    if (!handle) handle = fx->register_particle(spec);   /* one immediate retry */
    if (!handle) handle = fx->register_particle(spec);   /* and one more */
    particles[index] = handle;
}

/* All sprites obey the framework: <= 100 units, registration alpha <= 0x80. */
static void register_particles(void)
{
    MercFxParticle s;
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.max_distance = view_range;

    /* LIGHT: the M2 tactnuke light flash, white-blue and decaying fast
       (the story nuke's Light_large_whiteblue_cumana family). Spec matches
       the framework-proven flash particle exactly. */
    s.texture_hash = 0x88e27c45u;      /* retail flash sprite hash */
    s.blend_mode = 1;
    s.colors[0] = 0x80FFFFFFu; s.colors[1] = 0x60E0F0FFu; s.colors[2] = 0x00A0C4FFu;
    s.widths[0] = 30.0f;  s.heights[0] = 30.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 80.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.2f;
    s.y_acceleration = 2.0f; s.friction = 0.5f;
    s.lifetime = 0.8f; s.lifetime_random = 0.3f;
    register_particle_type(P_LIGHT, &s);

    /* GLARE: white-blue streaks shooting off the burst, cooling to pale
       blue over ~1.4 s. */
    s.texture_hash = 0x88e27c45u;
    s.blend_mode = 1;
    s.colors[0] = 0x80DCECFFu; s.colors[1] = 0x60A8C8FFu; s.colors[2] = 0x00708FB8u;
    s.widths[0] = 30.0f;  s.heights[0] = 30.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 80.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 1.0f; s.friction = 0.45f;
    s.lifetime = 1.4f; s.lifetime_random = 0.6f;
    register_particle_type(P_GLARE, &s);

    /* FIREBALL: the orange billowing fireball of the M2 detonation, white
       core to deep orange, gone in 3.5 s. */
    s.texture_hash = 0x02bb1fe0u;      /* retail explosion sprite hash */
    s.blend_mode = 1;
    s.colors[0] = 0x80FFF3B0u; s.colors[1] = 0x70F08A2Au; s.colors[2] = 0x30C05414u;
    s.widths[0] = 24.0f;  s.heights[0] = 24.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 60.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 2.5f; s.friction = 0.35f;
    s.lifetime = 2.2f; s.lifetime_random = 0.7f;
    s.random_scale = 0.4f;
    register_particle_type(P_FIREBALL, &s);

    /* FIRECAP: the burning cap. The M2 photo shows the WHOLE mushroom on
       fire: a self-lit additive orange cloud, bright enough to read through
       the distance haze from across the map. */
    s.texture_hash = 0x02bb1fe0u;
    s.blend_mode = 1;
    s.colors[0] = 0x80FFD960u; s.colors[1] = 0x70F07018u; s.colors[2] = 0x30B03C08u;
    s.widths[0] = 30.0f;  s.heights[0] = 30.0f;
    s.widths[1] = 70.0f;  s.heights[1] = 70.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 2.0f; s.friction = 0.3f;
    s.lifetime = 3.0f; s.lifetime_random = 1.0f;
    s.random_scale = 0.45f;
    register_particle_type(P_FIRECAP, &s);

    /* SHOCK: the flat grey dust ring of the M2 ground shockwave, wide and
       low, racing outward. */
    s.texture_hash = 0xc272d7afu;      /* retail smoke sprite hash */
    s.blend_mode = 0;
    s.colors[0] = 0x48EDEDEAu; s.colors[1] = 0x30D8D8D4u; s.colors[2] = 0x00B8B8B4u;
    s.widths[0] = 40.0f;  s.heights[0] = 12.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 26.0f;
    s.widths[2] = 100.0f; s.heights[2] = 44.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 0.2f; s.friction = 0.12f;
    s.lifetime = 2.2f; s.lifetime_random = 0.5f;
    s.wind = 1.0f;
    register_particle_type(P_SHOCK, &s);

    /* CAP: the dark smoke of the M2 cloud (the black collar between the
       burning cap and the stem). */
    s.texture_hash = 0xc272d7afu;
    s.blend_mode = 0;
    s.colors[0] = 0x78565048u; s.colors[1] = 0x584A443Cu; s.colors[2] = 0x00302C28u;
    s.widths[0] = 40.0f;  s.heights[0] = 35.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 60.0f;
    s.widths[2] = 100.0f; s.heights[2] = 85.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 2.0f; s.friction = 0.2f;
    s.lifetime = 9.0f; s.lifetime_random = 2.5f;
    s.random_scale = 0.35f; s.random_acceleration = 1.2f;
    s.wind = 1.0f;
    register_particle_type(P_CAP, &s);

    /* STEM: the thick grey column down to the ground. */
    s.texture_hash = 0xc272d7afu;
    s.blend_mode = 0;
    s.colors[0] = 0x686B665Eu; s.colors[1] = 0x48555048u; s.colors[2] = 0x003A3833u;
    s.widths[0] = 25.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 80.0f;
    s.widths[2] = 95.0f;  s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 2.0f; s.friction = 0.25f;
    s.lifetime = 10.0f; s.lifetime_random = 3.0f;
    register_particle_type(P_STEM, &s);

    /* DUST: the grey air smoke thinning out late in the show. */
    s.texture_hash = 0xc272d7afu;
    s.blend_mode = 0;
    s.colors[0] = 0x407E7A72u; s.colors[1] = 0x28A8A49Cu; s.colors[2] = 0x00C8C4BCu;
    s.widths[0] = 60.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 90.0f;  s.heights[1] = 70.0f;
    s.widths[2] = 100.0f; s.heights[2] = 95.0f;
    s.time_b = 0.4f;
    s.y_acceleration = 0.5f; s.friction = 0.15f;
    s.lifetime = 16.0f; s.lifetime_random = 4.0f;
    s.wind = 2.5f;
    register_particle_type(P_DUST, &s);

    /* BASE: the grey dust skirt around the stem foot. */
    s.texture_hash = 0xc272d7afu;
    s.blend_mode = 0;
    s.colors[0] = 0x506E6A60u; s.colors[1] = 0x40565050u; s.colors[2] = 0x00282420u;
    s.widths[0] = 40.0f;  s.heights[0] = 12.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 20.0f;
    s.widths[2] = 100.0f; s.heights[2] = 32.0f;
    s.time_b = 0.35f;
    s.y_acceleration = 1.0f; s.friction = 0.3f;
    s.lifetime = 12.0f; s.lifetime_random = 3.0f;
    s.wind = 2.0f;
    register_particle_type(P_BASE, &s);
}

static void emit_alpha(unsigned type, const float p[3], float vx, float vy, float vz,
                       float alpha, unsigned *seed);

static void emit(unsigned type, const float p[3], float vx, float vy, float vz,
                 unsigned *seed)
{
    emit_alpha(type, p, vx, vy, vz, 1.0f, seed);
}

static void emit_alpha(unsigned type, const float p[3], float vx, float vy, float vz,
                       float alpha, unsigned *seed)
{
    float velocity[3];
    float spin;
    if (type >= P_COUNT || !particles[type] || !fx || !fx->emit_particle) return;
    velocity[0] = vx; velocity[1] = vy; velocity[2] = vz;
    spin = (rand_frac(seed) - 0.5f) * 2.0f;
    fx->emit_particle(particles[type], p, velocity, spin, alpha);
}


static float terrain_at(float x, float z, float fallback)
{
    float h;
    if (fx && fx->sample_terrain_height && fx->sample_terrain_height(x, z, &h))
        return h;
    return fallback;
}

/* ------------------------------------------------- the M2 script emitters */

/* The M2 script's t0 line: fx("global_particle_airstrike_tactnuke"). */
static void script_detonate(Show *s)
{
    unsigned i, seed = s->seed ^ 0x4E554B45u;   /* 'NUKE' */
    float p[3];

    /* the white-blue light flash at the core */
    for (i = 0; i < 3; ++i) {
        p[0] = s->pos[0];
        p[1] = s->pos[1] + 4.0f + 12.0f * (float)i;
        p[2] = s->pos[2];
        emit(P_LIGHT, p, 0.0f, 0.0f, 0.0f, &seed);
    }
    /* the white-blue glare streaks shooting off the burst */
    for (i = 0; i < 8; ++i) {
        float angle = TAU * rand_frac(&seed);
        float radius = 8.0f + 55.0f * rand_frac(&seed);
        p[0] = s->pos[0] + cosf(angle) * radius;
        p[1] = s->pos[1] + 8.0f + 55.0f * rand_frac(&seed);
        p[2] = s->pos[2] + sinf(angle) * radius;
        emit(P_GLARE, p, cosf(angle) * 9.0f, 3.0f, sinf(angle) * 9.0f, &seed);
    }
    /* the orange fireball opening at ground zero: the whole M2 cloud starts
       as fire (the photo), so open BIG and BRIGHT */
    for (i = 0; i < 18; ++i) {
        float angle = TAU * rand_frac(&seed);
        float radius = 34.0f * rand_frac(&seed);
        p[0] = s->pos[0] + cosf(angle) * radius;
        p[1] = s->pos[1] + 3.0f + 34.0f * rand_frac(&seed);
        p[2] = s->pos[2] + sinf(angle) * radius;
        emit(P_FIREBALL, p, cosf(angle) * 8.0f, 7.0f + 5.0f * rand_frac(&seed),
             sinf(angle) * 8.0f, &seed);
    }
    /* the grey dust dome kicked up at the base */
    for (i = 0; i < 16; ++i) {
        float angle = TAU * rand_frac(&seed);
        float speed = 8.0f + 16.0f * rand_frac(&seed);
        p[0] = s->pos[0];
        p[1] = s->pos[1];
        p[2] = s->pos[2];
        emit(P_CAP, p, cosf(angle) * speed, 2.0f + 2.0f * rand_frac(&seed),
             sinf(angle) * speed, &seed);
    }
}

/* The M2 script's after(1.0, ...) line:
   fx("global_particle_exp_shockwave_ground_tactnuke"). One flat grey ring
   hugging the ground, racing outward, no spin, gone in seconds. */
static void script_shockwave(Show *s, float dt)
{
    const float ta = s->age - SHOCK_DELAY;   /* time since the script fired it */
    float radius;
    unsigned j;

    if (!cfg_shock) return;
    if (ta < 0.0f) return;
    radius = 20.0f + ta * 110.0f;
    if (radius >= SHOCK_RADIUS) return;
    s->next_shock -= dt;
    if (s->next_shock > 0.0f) return;
    s->next_shock = 0.1f;

    for (j = 0; j < 20; ++j) {
        float angle = (TAU * (float)j) / 20.0f + (rand_frac(&s->seed) - 0.5f) * 0.1f;
        float jitter = radius + (rand_frac(&s->seed) - 0.5f) * 20.0f;
        float x = s->pos[0] + cosf(angle) * jitter;
        float z = s->pos[2] + sinf(angle) * jitter;
        float ground = terrain_at(x, z, s->base_y);
        float p[3];
        p[0] = x; p[2] = z;
        p[1] = ground + 3.0f;                       /* the flat front */
        emit(P_SHOCK, p, cosf(angle) * 55.0f, 0.8f, sinf(angle) * 55.0f, &s->seed);
        p[1] = ground + 8.0f;                       /* the backing dust */
        emit(P_DUST, p, cosf(angle) * 35.0f, 1.2f, sinf(angle) * 35.0f, &s->seed);
    }
}

/* The orange fireball rolling outward and up while it burns out. */
static void show_fireball(Show *s, float dt)
{
    const float fade = s->age > 1.5f
        ? 1.0f - (s->age - 1.5f) / (FIREBALL_TIME - 1.5f) * 0.85f
        : 1.0f;
    unsigned j;

    if (!cfg_fire) return;
    if (s->age > FIREBALL_TIME) return;
    s->next_fire -= dt;
    if (s->next_fire > 0.0f) return;
    s->next_fire = 0.12f;

    for (j = 0; j < 3; ++j) {
        float angle = TAU * rand_frac(&s->seed);
        float radius = (4.0f + s->age * 14.0f) * (0.35f + 0.65f * rand_frac(&s->seed));
        float p[3];
        p[0] = s->pos[0] + cosf(angle) * radius;
        p[1] = s->pos[1] + 3.0f + 22.0f * rand_frac(&s->seed) + s->age * 3.0f;
        p[2] = s->pos[2] + sinf(angle) * radius;
        emit_alpha(P_FIREBALL, p, cosf(angle) * 5.0f, 5.0f, sinf(angle) * 5.0f,
                   fade, &s->seed);
    }
}

/* The M2 mushroom per the photo: a HUGE burning fireball cap over a dark
   collar, with a thick dark stem that carries fire inside it. The whole
   cloud burns for FIRE_TIME seconds, which is what makes it visible from
   across the map like in M2. */
static float cap_deck(const Show *s)
{
    float rise = s->age * (CAP_RISE_MAX / CAP_TOP_TIME);
    if (rise > CAP_RISE_MAX) rise = CAP_RISE_MAX;
    return CAP_HEIGHT + rise;
}

static void show_mushroom(Show *s, float dt)
{
    const float deck = cap_deck(s);
    const float unfurl = s->age < CAP_TOP_TIME ? s->age / CAP_TOP_TIME : 1.0f;
    const float fade = s->age > SHOW_DURATION * 0.6f
        ? 1.0f - (s->age - SHOW_DURATION * 0.6f) / (SHOW_DURATION * 0.4f) * 0.6f
        : 1.0f;
    /* the fire dies down over FIRE_TIME; the photo catches it mid-burn */
    const float fire_fade = s->age < FIRE_TIME
        ? 1.0f - (s->age / FIRE_TIME) * 0.5f
        : 0.0f;
    unsigned j, n = (s->age > SHOW_DURATION * 0.7f) ? 10u : 16u;

    s->next_cap -= dt;
    if (s->next_cap > 0.0f) return;
    s->next_cap = 0.1f;
    if (s->age > SHOW_DURATION * 0.85f) return;

    for (j = 0; j < n; ++j) {
        float angle = TAU * rand_frac(&s->seed);
        float radius = CAP_RADIUS * unfurl * sqrtf(rand_frac(&s->seed));
        float p[3];
        p[0] = s->pos[0] + cosf(angle) * radius;
        p[2] = s->pos[2] + sinf(angle) * radius;
        if (j < n - 4) {
            /* the burning cap: additive fire over the dark smoke body */
            p[1] = s->pos[1] + deck - 10.0f + 22.0f * rand_frac(&s->seed);
            emit_alpha(P_CAP, p, cosf(angle) * 2.0f, 1.2f, sinf(angle) * 2.0f,
                       fade * 0.9f, &s->seed);
            if (fire_fade > 0.0f)
                emit_alpha(P_FIRECAP, p, cosf(angle) * 2.0f, 2.0f,
                           sinf(angle) * 2.0f, fire_fade, &s->seed);
        } else {
            /* the dark collar under the head */
            radius = CAP_RADIUS * 1.25f * unfurl * sqrtf(rand_frac(&s->seed));
            p[0] = s->pos[0] + cosf(angle) * radius;
            p[2] = s->pos[2] + sinf(angle) * radius;
            p[1] = s->pos[1] + deck - 18.0f + 8.0f * rand_frac(&s->seed);
            emit_alpha(P_CAP, p, cosf(angle) * 1.5f, 0.6f, sinf(angle) * 1.5f,
                       fade * 0.95f, &s->seed);
            if (fire_fade > 0.0f && (j & 1) == 0)
                emit_alpha(P_FIRECAP, p, cosf(angle) * 1.5f, 1.0f,
                           sinf(angle) * 1.5f, fire_fade * 0.8f, &s->seed);
        }
    }

    /* the foot flames under the cap for the first seconds */
    if (s->age < FOOT_FIRE_TIME) {
        for (j = 0; j < 3; ++j) {
            float angle = TAU * rand_frac(&s->seed);
            float radius = CAP_RADIUS * unfurl * (0.25f + 0.6f * rand_frac(&s->seed));
            float p[3];
            p[0] = s->pos[0] + cosf(angle) * radius;
            p[2] = s->pos[2] + sinf(angle) * radius;
            p[1] = s->pos[1] + deck - 20.0f + 6.0f * rand_frac(&s->seed);
            emit_alpha(P_FIREBALL, p, cosf(angle) * 1.0f, 2.0f, sinf(angle) * 1.0f,
                       0.4f, &s->seed);
        }
    }
}

/* The thick dark stem down to the ground, carrying fire inside it while the
   cloud burns (the photo: an orange-lit column). */
static void show_stem(Show *s, float dt)
{
    const float fire_fade = s->age < FIRE_TIME ? 0.5f - (s->age / FIRE_TIME) * 0.35f : 0.0f;
    unsigned j;

    s->next_stem -= dt;
    if (s->next_stem > 0.0f) return;
    s->next_stem = 0.13f;
    if (s->age > SHOW_DURATION * 0.85f) return;

    for (j = 0; j < 5; ++j) {
        float span = cap_deck(s) - 10.0f;
        float frac = ((float)((unsigned)(rand_frac(&s->seed) * 5.0f)) / 5.0f);
        float radius = 60.0f - 34.0f * frac + 8.0f * rand_frac(&s->seed);
        float angle = TAU * rand_frac(&s->seed);
        float p[3];
        p[0] = s->pos[0] + cosf(angle) * radius;
        p[1] = s->base_y + 3.0f + frac * span;
        p[2] = s->pos[2] + sinf(angle) * radius;
        emit_alpha(P_STEM, p, cosf(angle) * 0.4f, 3.0f + 2.0f * rand_frac(&s->seed),
                   sinf(angle) * 0.4f, 0.9f, &s->seed);
        /* the fire core inside the stem */
        if (fire_fade > 0.0f && (j & 1) == 0) {
            p[0] = s->pos[0] + cosf(angle) * radius * 0.45f;
            p[2] = s->pos[2] + sinf(angle) * radius * 0.45f;
            emit_alpha(P_FIRECAP, p, cosf(angle) * 0.5f, 3.0f, sinf(angle) * 0.5f,
                       fire_fade, &s->seed);
        }
    }
}

/* The grey dust skirt around the foot plus the late air smoke. */
static void show_dust(Show *s, float dt)
{
    unsigned j;

    s->next_dust -= dt;
    if (s->next_dust > 0.0f) return;
    s->next_dust = 0.3f;

    if (s->age < 25.0f) {
        /* the skirt grows over the area */
        for (j = 0; j < 3; ++j) {
            float angle = TAU * rand_frac(&s->seed) + s->age * 0.4f;
            float radius = (25.0f + s->age * 40.0f) * sqrtf(rand_frac(&s->seed));
            if (radius > DUST_CAP) radius = DUST_CAP * sqrtf(rand_frac(&s->seed));
            float x = s->pos[0] + cosf(angle) * radius;
            float z = s->pos[2] + sinf(angle) * radius;
            float ground = terrain_at(x, z, s->base_y);
            float p[3];
            p[0] = x; p[2] = z;
            p[1] = ground + 1.5f;
            emit(P_BASE, p, cosf(angle) * 8.0f, 1.5f, sinf(angle) * 8.0f, &s->seed);
        }
    }
    if (s->age > 8.0f) {
        /* the late grey air smoke */
        for (j = 0; j < 2; ++j) {
            float angle = TAU * rand_frac(&s->seed);
            float radius = 60.0f + 100.0f * rand_frac(&s->seed);
            float p[3];
            p[0] = s->pos[0] + cosf(angle) * radius;
            p[1] = s->pos[1] + 40.0f + 80.0f * rand_frac(&s->seed);
            p[2] = s->pos[2] + sinf(angle) * radius;
            emit(P_DUST, p, cosf(angle) * 2.0f, 1.0f, sinf(angle) * 2.0f, &s->seed);
        }
    }
}

/* --------------------------------------------------- the script timeline */

/* THE M2 SCRIPT AS DATA - the playFX("NUKE") sequence, verbatim: the tactnuke
   FX at t0, the ground shockwave FX at after(1.0). */
enum { EV_DETONATE = 1, EV_SHOCKWAVE = 2 };
typedef struct M2ScriptEvent {
    float t;
    unsigned char id;
} M2ScriptEvent;
static const M2ScriptEvent m2_script[] = {
    { 0.0f, EV_DETONATE },
    { 1.0f, EV_SHOCKWAVE },
};
#define M2_SCRIPT_LEN (sizeof(m2_script) / sizeof(m2_script[0]))

static void __cdecl show_tick(float delta_seconds, void *userdata)
{
    unsigned i;
    (void)userdata;

    if (!fx) return;
    for (i = 0; i < SHOW_COUNT; ++i) {
        Show *s = &shows[i];
        if (!s->active) continue;
        s->age += delta_seconds;

        /* run the M2 script events whose time has come */
        while (s->script_next < M2_SCRIPT_LEN && s->age >= m2_script[s->script_next].t) {
            switch (m2_script[s->script_next].id) {
            case EV_DETONATE:  script_detonate(s); break;
            case EV_SHOCKWAVE: s->next_shock = 0.0f; break;   /* emitter takes it */
            default: break;
            }
            ++s->script_next;
        }

        script_shockwave(s, delta_seconds);
        show_fireball(s, delta_seconds);
        show_mushroom(s, delta_seconds);
        show_stem(s, delta_seconds);
        show_dust(s, delta_seconds);

        if (s->age >= SHOW_DURATION + 14.0f) {   /* let the last smoke drift */
            memset(s, 0, sizeof(*s));
        }
    }
}

static void __cdecl show_reset(void *userdata)
{
    (void)userdata;
    memset(shows, 0, sizeof(shows));
    flash_time = -1.0f;
}

/* -------------------------------------------------------- deferred blast */

static float deton_pos[3], deton_radius, deton_force, deton_damage;
static int deton_pending, deton_go, explosion_ok;

static void quake_opts(MercFxBlastOptions *options)
{
    if (cfg_quake == 1) { options->shake_duration = 10.0f; options->shake_magnitude = 3.0f; }
    else if (cfg_quake == 2) { options->shake_duration = 18.0f; options->shake_magnitude = 4.0f; }
}

/* --------------------------------------------------------- impact handler */

static void flash_whiteout(void)
{
    if (!cfg_whiteout || !flash_texture) return;
    flash_time = 0.0f;
}

static MercFxDecision __cdecl tactnuke_impact(const MercFxImpact *event, void *userdata)
{
    MercFxImpact *ev = (MercFxImpact *)event;   /* framework-owned copy */
    MercFxBlastOptions options;
    Show *s;
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

    /* the M2 tactical nuke erases its target area */
    ev->splash_damage *= DAMAGE_SCALE;
    ev->splash_force *= FORCE_SCALE;
    ev->splash_radius = SPLASH_RADIUS;

    if (explosion_ok) {
        deton_pos[0] = event->position[0];
        deton_pos[1] = event->position[1];
        deton_pos[2] = event->position[2];
        deton_radius = ev->splash_radius;
        deton_force = ev->splash_force;
        deton_damage = ev->splash_damage;
        deton_pending = 1;
        deton_go = 1;                       /* the M2 bomb goes on impact */
    } else {
        /* no engine access: fall back to the instant framework blast */
        memset(&options, 0, sizeof(options));
        options.size = sizeof(options);
        /* M2 has no red shockwave: the plain explosion damage type only. */
        options.damage_type = MERCFX_BLAST_STANDARD;
        options.scorch_radius = 40.0f;
        quake_opts(&options);
        if (cfg_audio == 2 && sound_primary && fx->sound_ready &&
            fx->sound_ready(sound_primary))
            options.suppress_default_sound = 1u;      /* NUKE ONLY */
        if (!fx->apply_blast_configured(event, &options)) {
            LOG("blast rejected by the framework; the vanilla explosion took over");
            return MERCFX_UNHANDLED;
        }
    }

    /* start the M2 script show */
    s = grab_show();
    memset(s, 0, sizeof(*s));
    s->active = 1;
    s->age = 0.0f;
    s->script_next = 0;                     /* the script plays from t0 */
    s->pos[0] = event->position[0];
    s->pos[1] = event->position[1] + 2.0f;  /* the M2 detonation point: ground */
    s->pos[2] = event->position[2];
    s->base_y = event->position[1];
    s->seed = 0x4D324E4Bu ^ (unsigned)(int)(event->position[0] + event->position[1]
                                           + event->position[2]);   /* 'M2NU' */
    s->next_fire = 0.0f;
    s->next_shock = 0.0f;
    s->next_cap = 0.05f;
    s->next_stem = 0.10f;
    s->next_dust = 0.25f;

    if (cfg_audio == 1 && sound_primary && fx->sound_ready &&
        fx->sound_ready(sound_primary))
        fx->play_sound(sound_primary, s->pos);
    if (fx->request_slowdown) fx->request_slowdown(MOD_OWNER, 0.6f, 1.0f);
    flash_whiteout();

    LOG("tactnuke: r=%.0f at %.0f %.0f %.0f",
        event->splash_radius, event->position[0], event->position[1],
        event->position[2]);
    return MERCFX_REMOVE;
}

/* ------------------------------------------------------------------ hooks */

static void draw_flash(uint32_t brush)
{
    float t = flash_time;
    float alpha, per_brush;
    uint32_t color, begin_args[5], quad_args[8];
    unsigned n, pass;

    if (!flash_texture || !have_begin || !have_quad) return;
    n = brushes_last_frame;
    if (!n || n > MAX_BRUSHES) return;

    if (t < FLASH_HOLD) {
        alpha = 1.0f;
    } else {
        float total = t - FLASH_HOLD;
        if (total >= FLASH_FADE) { flash_time = -1.0f; return; }
        alpha = powf(1.0f - total / FLASH_FADE, 1.6f);
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

static void try_stock_item(void);
static void try_attach(void);

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
        static float rebind_timer;
        rebind_timer += delta;
        if (rebind_timer >= 2.0f) {
            unsigned i, ok = 0;
            rebind_timer = 0.0f;
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
        if (flash_time > FLASH_HOLD + FLASH_FADE + 1.0f)
            flash_time = -1.0f;
    }

    /* the M2 bomb goes on impact: the damage lands at once */
    if (deton_pending && deton_go && explosion_ok) {
        uint32_t args[13];
        memcpy(args, deton_pos, 12);
        args[3] = float_bits(deton_radius);   /* blast radius */
        args[4] = float_bits(deton_force);    /* force */
        args[5] = 0u;                         /* min damage */
        args[6] = float_bits(deton_damage);   /* max damage */
        args[7] = 0u;                         /* no land visual: our show plays */
        args[8] = 0u;                         /* our cue replaces the stock sound */
        /* damage type 0: the plain explosion. M2 has no red shockwave and
           neither does this mod; type 3 would draw the retail red decal. */
        args[9] = 0u;
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
        if (fx->unregister_simulation) fx->unregister_simulation(show_tick, NULL);
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
        host->register_string(SHOP_NAME_KEY, "MERC 2 NUKE BUNKER BUSTER");
        host->register_string(SHOP_SHORT_KEY, "The Mercenaries 2 tactical nuke.");
        host->register_string(SHOP_LONG_KEY,
            "Straight from MERCENARIES 2: WORLD IN FLAMES, transcribed from the "
            "M2 script: the bomb falls and goes on impact with a white-blue "
            "flash and an orange fireball, ONE SECOND later the big grey "
            "ground shockwave ring races out, then the squat wide M2 mushroom "
            "rises over a thick stem. No red shockwave. Leave nothing standing.");
    }
    if (host->add_shop_item(&shop_item))
        LOG("shop item MERC 2 NUKE BUNKER BUSTER added on %s", SHOP_TEMPLATE);
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
    LOG("stocked MERC 2 NUKE BUNKER BUSTER in the Merchant of Menace");
    next_stock_time = world_time + 30.0;
}

/* --------------------------------------------------------------- settings */

static unsigned get_master(void) { return cfg_master; }
static unsigned get_shock(void)  { return cfg_shock; }
static unsigned get_fire(void)   { return cfg_fire; }
static unsigned get_quake(void)  { return cfg_quake; }
static unsigned get_audio(void)  { return cfg_audio; }
static unsigned get_whiteout(void) { return cfg_whiteout; }
static unsigned get_debug(void)  { return cfg_debug; }

static void write_unsigned(const char *key, unsigned value)
{
    if (host && host->write_setting) host->write_setting(MOD_INI, MOD_SECTION, key, value);
}

#define SET_on(name, var) \
    static void set_##var(unsigned v) { cfg_##var = v & 1u; write_unsigned(name, cfg_##var); }

SET_on("Master", master)
SET_on("Shockwave", shock)
SET_on("Fireball", fire)
SET_on("Whiteout", whiteout)
SET_on("DebugLog", debug)

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
static const char *const quake_labels[] = {"OFF", "STRONG", "EXTREME"};
static const char *const audio_labels[] = {"OFF", "LAYERED", "NUKE ONLY"};

#define SETTING_COUNT 7u

static const MercenariesModSetting settings[SETTING_COUNT] = {
    {"MASTER",     on_off, 2, get_master, set_master},
    {"SHOCKWAVE",  on_off, 2, get_shock,  set_shock},
    {"FIREBALL",   on_off, 2, get_fire,   set_fire},
    {"EARTHQUAKE", quake_labels, 3, get_quake, set_quake},
    {"NUKE AUDIO", audio_labels, 3, get_audio, set_audio},
    {"WHITEOUT",   on_off, 2, get_whiteout, set_whiteout},
    {"DEBUG LOG",  on_off, 2, get_debug,  set_debug},
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
    1u, 1u, 1u, 1u, 2u, 1u, 0u,
};

static const char *const setting_descriptions[SETTING_COUNT] = {
    "Turns the whole nuke on and off. OFF keeps every bomb vanilla.",
    "The M2 ground shockwave: the flat grey ring that fires exactly one "
    "second after the detonation, straight from the M2 script's after(1.0).",
    "The orange fireball opening at ground zero and burning out in 3.5 s.",
    "EARTHQUAKE shake after every nuke: STRONG 10 s or EXTREME 18 s.",
    "NUKE ONLY replaces the vanilla blast sound with the strike cue; LAYERED keeps both.",
    "The long white flash over the screen when the bomb lands, like the M2 flash.",
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
    "The MERCENARIES 2: WORLD IN FLAMES tactical nuke, transcribed from the "
    "M2 script: the bomb falls and goes on impact with a white-blue flash "
    "and an orange fireball, ONE SECOND later the big grey ground shockwave "
    "ring races out, then the squat wide M2 mushroom rises over a thick "
    "stem. No red shockwave, no Meltdown doom sphere. Needs Native Effects.",
    setting_descriptions,
    setting_controls,
    NULL,
    NULL,
    setting_defaults,
};

/* ---------------------------------------------------------------- attach */

static MercFxDecision __cdecl tactnuke_impact(const MercFxImpact *event, void *userdata);

static void try_attach(void)
{
    HMODULE module;
    MercFxGetApi get_api;
    unsigned i, ok_count = 0;

    if (attach_state == 3 || attach_state == 2) return;
    module = GetModuleHandleA("native_effects.dll");
    if (!module) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("native_effects.dll not loaded; MERC 2 NUKE BUNKER BUSTER stays vanilla");
        }
        return;
    }
    get_api = (MercFxGetApi)(void *)GetProcAddress(module, "MercFx_GetApi");
    if (!get_api) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("native_effects.dll has no MercFx_GetApi; staying vanilla");
        }
        return;
    }
    fx = get_api(MERCFX_API_VERSION);
    if (!fx || fx->version != MERCFX_API_VERSION) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("MercFx API version mismatch; staying vanilla");
        }
        return;
    }
    if (fx->size < offsetof(MercFxApi, register_sound) + sizeof(fx->register_sound) ||
        !fx->register_effect || !fx->bind_template || !fx->apply_blast_configured ||
        !fx->request_slowdown || !fx->register_simulation ||
        !fx->add_ground_scorch || !fx->register_particle || !fx->prepare_particle ||
        !fx->emit_particle || !fx->sample_terrain_height) {
        attach_state = 3;
        LOG("MercFx API table incomplete (%u bytes); staying vanilla", fx->size);
        return;
    }

    register_particles();
    for (i = 0; i < P_COUNT; ++i) {
        if (particles[i]) DLOG("particle %u of %u registered", i + 1u, (unsigned)P_COUNT);
        else LOG("particle %u of %u refused by the framework", i + 1u, (unsigned)P_COUNT);
    }

    if (!fx->register_effect(EFFECT_NAME, tactnuke_impact, NULL)) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("MercFx effect registration failed; staying vanilla");
        }
        return;
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

    if (!fx->register_simulation(show_tick, show_reset, NULL))
        LOG("script timeline registration failed; the show will not build");

    LOG("attached: bound=%u of %u, master=%u, quake=%u, audio=%u, whiteout=%u",
        bound_count, (unsigned)TRIGGER_COUNT, cfg_master, cfg_quake,
        cfg_audio, cfg_whiteout);
    if (!logged_advisory) {
        logged_advisory = 1;
        LOG("the meltdown warhead opens mid-air by engine script; order the "
            "MERC 2 NUKE BUNKER BUSTER airstrike from the shop for a doom-free nuke");
    }
}

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
    cfg_shock = host->read_setting(MOD_INI, MOD_SECTION, "Shockwave", cfg_shock) & 1u;
    cfg_fire = host->read_setting(MOD_INI, MOD_SECTION, "Fireball", cfg_fire) & 1u;
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
    if (v > 8192u) v = 8192u;   /* wide enough to see the cloud across the map */
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
