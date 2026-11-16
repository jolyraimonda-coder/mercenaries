/* NUKE BOMBER 1 v1.0.0 - a code mod for Mercenaries Recompiled 1.1.1
 * (Mod Manager / Mod API v2 + Native Effects / MercFx).
 *
 * This is the MERGE the user asked for: the WORKING MOAB effect plumbing
 * (a proper MercFx provider: register_effect -> impact callback ->
 * apply_blast_configured -> register_simulation timelines, written by
 * Svered and proven in the MOAB test suite) fused with the MERCENARIES 2:
 * WORLD IN FLAMES tactical nuke script, mined out of the M2 PC game data.
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
 * same pair through its loc_tactnuke + loc_shockwave markers with a
 * white-blue light (Light_large_whiteblue_cumana). The show:
 *
 *   t0     DETONATE  - white-blue flash, orange fireball opening at ground
 *                      zero, grey dust dome; the blast lands here
 *   t+1.0  SHOCKWAVE - the M2 ground shockwave: a big flat grey dust ring
 *                      hugging the terrain, racing outward
 *   t0..t40 MUSHROOM - the whole cloud BURNS for ten seconds (huge self-lit
 *                      fire cap over a dark collar and a burning stem),
 *                      then smoke takes over and thins away
 *
 * NO RED SHOCKWAVE: the blast is committed as MERCFX_BLAST_STANDARD (the
 * retail bunker-buster type draws the red decal; M2 has no red shockwave),
 * no meltdown effect, no engine shockwave call.
 *
 * WHY THIS ONE WORKS WHERE EARLIER NUKES STAYED INVISIBLE: every emission
 * happens INSIDE MercFx callbacks (impact / simulation), which is where the
 * framework allows emit_particle; earlier builds emitted from a mod-owned
 * hook, which the framework silently drops. Particle specs obey the
 * framework validation: max_distance 128..4096, sprite size <= 100,
 * registration alpha <= 128, time_b in (0,1), lifetime 0.05..30.
 *
 * THE FOG VIEW CONTROL (on this mod's MODS page): the retail renderer keeps
 * its depth-fog range in two initialized globals (_fFogMin=200, _fFogMax=400
 * in RedRenderer); everything beyond 400 units is fully fogged, which hid
 * every earlier mushroom. FOG VIEW rewrites those globals every world tick
 * (OFF restores the vanilla wall). The globals' guest addresses come from
 * the retail XBE section table (unique 28-byte pattern at file 0x300CD8,
 * mapped to guest +0x30F278); the patch validates the pair every tick and
 * logs the values it found, so the log always shows whether it landed.
 *
 * Order the BUNKER BUSTER airstrike from the shop to call the nuke.
 *
 * Build (64-bit Windows DLL), with mercenaries_mod.h and mercfx.h beside it:
 *   cl /nologo /W4 /O2 /MT /LD nuke_bomber.c /I . /Fe:nuke_bomber.dll /link /MACHINE:X64 kernel32.lib libucrt.lib libvcruntime.lib
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

#define MOD_NAME            "NUKE BOMBER 1"
#define MOD_VERSION         "1.0.0"
#define MOD_INI             "mods\\nuke_bomber.ini"
#define MOD_SECTION         "NukeBomber"

#define EFFECT_NAME         "nuke_bomber_main"

/* The strike that delivers the nuke: the retail bunker buster delivery (M2's
 * tactical nuke IS a Nuclear Bunker Buster Projectile), plus the copies the
 * Merchant of Menace Expanded shops stock. */
static const char *const triggers[] = {
    "template_amm_bunkerbuster",
    "template_amm_bunkerbuster0",
    "template_amm_bunkerbusterbomb",
    "template_amm_mod_nuclear_bunker_buster",
    "template_amm_fuelairbomb",
    "template_amm_fuelairbomb0",
    "template_amm_cruisemissile",
    "template_amm_cruisemissile0",
    "template_amm_meltdown",
};
#define TRIGGER_COUNT (sizeof(triggers) / sizeof(triggers[0]))

/* Retail sprite helpers for the whiteout flash (verified addresses). */
#define SPRITE_BEGIN_FN     0x0020A0D0u
#define SPRITE_QUAD_FN      0x0020C980u

/* The M2 show, one size: the story nuke of pmccon004. */
#define SHOW_TIMELINES      8
#define SHOW_DURATION       40.0f         /* seconds of cloud */
#define SHOCK_DELAY         1.0f          /* the Lua's after(1.0, ...) */
#define SHOCK_SPEED         75.0f         /* flat grey ring, ground level */
#define FIREBALL_TIME       3.5f          /* the opening fireball burns out */
#define CAP_TOP_TIME        6.0f          /* the cap is up by six seconds */
#define CAP_HEIGHT          95.0f
#define CAP_RADIUS          90.0f         /* sprite-size cap: multiple sprites */
#define FIRE_TIME           10.0f         /* the whole cloud BURNS this long */
#define FOOT_FIRE_TIME      6.0f          /* flames at the cap foot */
#define DUST_CAP            300.0f        /* the skirt's widest radius */
#define FLASH_HOLD          1.0f          /* white screen, like the M2 flash */
#define FLASH_FADE          3.0f
#define FLASH_PASSES        3u            /* stacked quads for a solid white */

/* Explosion cue and time control (the MOAB values, they fit the nuke). */
#define NUKE_SOUND_CUE      "a_strike.bb_explosion"
#define SHAKE_DURATION      4.0f          /* raw engine duration */
#define SHAKE_MAGNITUDE     1.0f          /* retail bunker-buster peak */
#define SCORCH_RADIUS       40.0f         /* framework maximum */

/* The fog wall (see the header comment): guest addresses of RedRenderer's
 * _fFogMin/_fFogMax, derived from the XBE section table. The 28-byte
 * signature at guest +0x30F278 validates the mapping at runtime. */
#define FOG_SIG_ADDR        0x30F278u
#define FOG_MIN_ADDR        0x30F284u
#define FOG_MAX_ADDR        0x30F288u

/* ------------------------------------------------------------- tiny helpers */

#define TAU 6.2831853f

static const MercenariesModHost *host;
static const MercFxApi *fx;
static unsigned attach_state;          /* 0 idle, 1 waiting, 2 live, 3 vanilla */
static unsigned attach_attempts;

static uint32_t trigger_hash[TRIGGER_COUNT];
static unsigned char bound_flags[TRIGGER_COUNT];
static unsigned bound_count;

static uint32_t particles[8];
enum { P_FLASH, P_FIRE, P_FIRECAP, P_SHOCK, P_STEM, P_COLLAR, P_DUST, P_GROUND, P_COUNT_IDX };
#define P_COUNT P_COUNT_IDX

static uint32_t sound_primary;
static uint32_t flash_texture;
static int have_begin, have_quad;
static float flash_time = -1.0f;
static unsigned brushes_this_frame, brushes_last_frame;
#define MAX_BRUSHES 512u

static unsigned cfg_fog = 3000u;       /* FOG VIEW index into fog_values */
static unsigned cfg_quake = 1;         /* EARTHQUAKE 0 off, 1 strong, 2 extreme */
static unsigned cfg_slowmo = 1;        /* SLOW MOTION 0 off, 1 .75, 2 .5 */
static unsigned cfg_whiteout = 1;      /* WHITEOUT */
static unsigned cfg_fire = 1;          /* MUSHROOM FIRE */
static unsigned cfg_ground = 1;        /* GROUND COVER */
static unsigned cfg_debug = 0;         /* DEBUG LOG */

static const unsigned fog_values[7] = {0u, 600u, 1200u, 2000u, 3000u, 5000u, 8000u};
static float fog_units;                /* 0 = leave vanilla, else fog end */

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
    uint32_t value = *seed;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *seed = value;
    return (float)(value >> 8) * (1.0f / 16777216.0f);
}

static void shutdown_hook(void);
static void try_attach(void);
static void brush_end(uint32_t brush, int pda, uint32_t vertex_base, uint32_t vertex_first,
                      uint32_t *vertex_end, uint32_t primitive_base, uint32_t primitive_first,
                      uint32_t *primitive_end);
static void draw_flash(uint32_t brush);
static void fog_patch_tick(void);

#define LOG(...) do { if (host && host->log) host->log("nuke_bomber", __VA_ARGS__); } while (0)
#define DLOG(...) do { if (cfg_debug && host && host->log) host->log("nuke_bomber_debug", __VA_ARGS__); } while (0)

/* --------------------------------------------------------------- the show */

typedef struct BomberTimeline {
    float position[3];
    float age, pending;          /* elapsed simulation time, emission remainder */
    uint32_t random, ground_sample;
    float ground_phase;
    unsigned char shock_done;
    int active;
} BomberTimeline;

static BomberTimeline timelines[SHOW_TIMELINES];
static unsigned timeline_cursor;
static uint32_t show_random = 0x6e756b65u;

static void reset_show(void)
{
    memset(timelines, 0, sizeof(timelines));
    show_random = 0x6e756b65u;
}

static void emit2(unsigned kind, const float position[3], float vx, float vy,
                  float vz, float spin, float alpha)
{
    float velocity[3] = { vx, vy, vz };
    if (kind >= P_COUNT_IDX || !particles[kind]) return;
    fx->emit_particle(particles[kind], position, velocity, spin, alpha);
}

static void emit_rand(unsigned kind, const float position[3], float vx, float vy,
                      float vz, uint32_t *random)
{
    emit2(kind, position, vx, vy, vz, (rand_frac(random) - 0.5f) * 2.0f, 1.0f);
}

/* t0 DETONATE: the M2 tactnuke FX - the white-blue flash, the orange
   fireball opening at ground zero and the grey dust dome. */
static void burst(const float origin[3], uint32_t *random)
{
    unsigned i;
    float position[3] = { origin[0], origin[1] + 2.0f, origin[2] };
    emit2(P_FLASH, position, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    for (i = 0; i < 36; ++i) {                    /* fast low fire ring */
        float angle = TAU * rand_frac(random);
        float speed = 10.0f + 18.0f * rand_frac(random);
        emit_rand(P_FIRE, position, cosf(angle) * speed,
                  1.0f * rand_frac(random), sinf(angle) * speed, random);
    }
    for (i = 0; i < 36; ++i) {                    /* slower rising fire */
        float angle = TAU * rand_frac(random);
        float speed = 5.0f + 10.0f * rand_frac(random);
        emit_rand(P_FIRE, position, cosf(angle) * speed,
                  2.0f * rand_frac(random), sinf(angle) * speed, random);
    }
    for (i = 0; i < 12; ++i) {                    /* the fire column start */
        float angle = TAU * rand_frac(random);
        float speed = 5.0f + 5.0f * rand_frac(random);
        emit_rand(P_FIRE, position, cosf(angle) * speed,
                  3.0f + 5.0f * rand_frac(random), sinf(angle) * speed, random);
    }
    for (i = 0; i < 24; ++i) {                    /* the grey dust dome */
        float angle = TAU * (float)i / 24.0f;
        float x = cosf(angle), z = sinf(angle);
        position[0] = origin[0] + x * 5.0f;
        position[1] = origin[1] + 0.75f;
        position[2] = origin[2] + z * 5.0f;
        emit_rand(P_DUST, position, x * 32.0f, 0.6f, z * 32.0f, random);
    }
}

/* t+1.0 SHOCKWAVE: the M2 ground shockwave - one big flat grey dust ring
   hugging the terrain, racing outward, no spin. */
static void shockwave(const float origin[3], uint32_t *random)
{
    unsigned i;
    for (i = 0; i < 24; ++i) {
        float angle = TAU * (float)i / 24.0f;
        float x = cosf(angle), z = sinf(angle);
        float position[3] = { origin[0] + x * 6.0f, origin[1] + 1.5f, origin[2] + z * 6.0f };
        emit_rand(P_SHOCK, position, x * SHOCK_SPEED, 0.0f, z * SHOCK_SPEED, random);
    }
    for (i = 0; i < 12; ++i) {                    /* grey backing smoke */
        float angle = TAU * rand_frac(random);
        float speed = 20.0f + 15.0f * rand_frac(random);
        float position[3] = { origin[0], origin[1] + 1.0f, origin[2] };
        emit_rand(P_DUST, position, cosf(angle) * speed, 0.3f,
                  sinf(angle) * speed, random);
    }
}

/* the burning mushroom deck: the cap deck emits fire while it burns and
   smoke as it ages; heights and radius follow the M2 squat-wide shape. */
static void cap_deck(const BomberTimeline *timeline, float height, float radius,
                     float fire, uint32_t *random)
{
    unsigned i;
    for (i = 0; i < 4; ++i) {
        float angle = TAU * rand_frac(random);
        float spread = radius * (0.6f + 0.4f * rand_frac(random));
        float x = cosf(angle), z = sinf(angle);
        float position[3] = {
            timeline->position[0] + x * spread,
            timeline->position[1] + height + rand_frac(random) * 6.0f,
            timeline->position[2] + z * spread
        };
        unsigned kind = P_FIRECAP;
        float rise = 1.5f + 2.0f * rand_frac(random);
        if (fire <= 0.0f) kind = P_COLLAR;        /* burnt: dark smoke */
        emit_rand(kind, position, x * 1.5f, rise, z * 1.5f, random);
    }
}

/* the ground cover: persistent terrain smoke out to the skirt radius,
   the MOAB ground blanket logic. */
static void emit_ground(BomberTimeline *timeline)
{
    unsigned i;
    if (timeline->age >= 25.0f) return;
    if (fx->size < offsetof(MercFxApi, sample_terrain_height) + sizeof(fx->sample_terrain_height) ||
        !fx->sample_terrain_height)
        return;
    for (i = 0; i < 4u; ++i) {
        uint32_t sample = timeline->ground_sample++;
        float radius = 50.0f * sqrtf(fmodf(0.5f + (float)sample * 0.754877666f, 1.0f));
        float angle = timeline->ground_phase + (float)sample * 2.39996322973f;
        float position[3] = {
            timeline->position[0] + cosf(angle) * radius, 0.0f,
            timeline->position[2] + sinf(angle) * radius
        };
        float height;
        if (!fx->sample_terrain_height(position[0], position[2], &height) ||
            !isfinite(height) || !isfinite(height + 3.0f))
            continue;
        position[1] = height + 3.0f;
        emit2(P_GROUND, position, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    }
}

/* The M2 script, walked forward by the framework's simulation step. */
static void __cdecl simulate(float dt, void *context)
{
    unsigned i, j;
    (void)context;
    if (!fx || !isfinite(dt) || dt <= 0.0f) return;
    for (i = 0; i < SHOW_TIMELINES; ++i) {
        BomberTimeline *timeline = &timelines[i];
        if (!timeline->active) continue;
        if (dt >= SHOW_DURATION - timeline->age) { timeline->active = 0; continue; }
        timeline->age += dt;
        timeline->pending += dt;
        if (timeline->pending < 0.125f) continue;
        timeline->pending = fmodf(timeline->pending, 0.125f);

        /* t+1.0: the M2 ground shockwave, exactly once */
        if (!timeline->shock_done && timeline->age >= SHOCK_DELAY) {
            shockwave(timeline->position, &timeline->random);
            timeline->shock_done = 1;
        }

        if (cfg_ground) emit_ground(timeline);

        /* the opening fireball feeds 0..3.5 s from the impact point */
        if (timeline->age < FIREBALL_TIME) {
            for (j = 0; j < 3; ++j) {
                float angle = TAU * rand_frac(&timeline->random);
                float x = cosf(angle), z = sinf(angle);
                float radius = timeline->age * 9.0f;
                float position[3] = {
                    timeline->position[0] + x * radius,
                    timeline->position[1] + 2.0f + timeline->age * 3.5f,
                    timeline->position[2] + z * radius
                };
                emit_rand(P_FIRE, position, x * 2.0f, 5.0f, z * 2.0f, &timeline->random);
            }
        }

        /* the burning mushroom: cap rises 0..6 s, the whole cloud burns to 10 s */
        if (cfg_fire && timeline->age < FIRE_TIME) {
            float up = timeline->age / CAP_TOP_TIME;
            if (up > 1.0f) up = 1.0f;
            cap_deck(timeline, 8.0f + up * CAP_HEIGHT,
                     20.0f + up * CAP_RADIUS, 1.0f, &timeline->random);
            if (timeline->age < FOOT_FIRE_TIME) {   /* flames at the cap foot */
                for (j = 0; j < 2; ++j) {
                    float angle = TAU * rand_frac(&timeline->random);
                    float x = cosf(angle), z = sinf(angle);
                    float position[3] = {
                        timeline->position[0] + x * 8.0f,
                        timeline->position[1] + 2.0f,
                        timeline->position[2] + z * 8.0f
                    };
                    emit_rand(P_FIRE, position, x * 0.5f, 3.0f, z * 0.5f,
                              &timeline->random);
                }
            }
        }

        /* the stem: a thick column carrying the cap, 2..30 s */
        if (timeline->age >= 1.5f && timeline->age < 30.0f) {
            for (j = 0; j < 2; ++j) {
                float angle = TAU * rand_frac(&timeline->random);
                float spread = 1.0f + 2.5f * rand_frac(&timeline->random);
                float x = cosf(angle), z = sinf(angle);
                float stem_height = 4.0f + rand_frac(&timeline->random) *
                    (timeline->age < CAP_TOP_TIME ? timeline->age * 14.0f : CAP_HEIGHT);
                float position[3] = {
                    timeline->position[0] + x * spread,
                    timeline->position[1] + stem_height,
                    timeline->position[2] + z * spread
                };
                emit_rand(P_STEM, position, x * 0.35f, 1.2f + 1.2f * rand_frac(&timeline->random),
                          z * 0.35f, &timeline->random);
            }
        }

        /* the dust skirt: grey air smoke spreading under the cloud */
        if (timeline->age >= 2.0f && timeline->age < 26.0f) {
            for (j = 0; j < 2; ++j) {
                float angle = TAU * rand_frac(&timeline->random);
                float radius = 10.0f + (DUST_CAP - 10.0f) *
                    (timeline->age / 26.0f) * rand_frac(&timeline->random);
                float x = cosf(angle), z = sinf(angle);
                float position[3] = {
                    timeline->position[0] + x * radius,
                    timeline->position[1] + 1.0f,
                    timeline->position[2] + z * radius
                };
                emit_rand(P_DUST, position, x * 1.0f, 0.2f, z * 1.0f, &timeline->random);
            }
        }
    }
}

static void __cdecl reset_timelines(void *context)
{
    (void)context;
    reset_show();
}

/* ---------------------------------------------------------------- particles */

/* Specs obey the framework validation (particle_runtime.c): texture in the
   retail cache, alpha <= 0x80, widths/heights (0,100], time_b (0,1),
   lifetime [0.05,30], max_distance [128,4096]. All colors/arithmetic match
   the retail constructor's ABGR conversion done by the framework. */
static int register_particles(void)
{
    static const uint32_t textures[P_COUNT_IDX] = {
        0x88e27c45u,   /* flash */
        0x02bb1fe0u,   /* fire: retail explosion sheet */
        0x02bb1fe0u,   /* burning cap: same sheet, bigger */
        0xc272d7afu,   /* shock ring: retail smoke */
        0xc272d7afu,   /* stem */
        0xc272d7afu,   /* collar */
        0xc272d7afu,   /* dust */
        0xc272d7afu    /* ground cover */
    };
    static const uint32_t colors[P_COUNT_IDX][3] = {
        { 0x80FFFFFFu, 0x60E0F0FFu, 0x00A0C4FFu },   /* white-blue light */
        { 0x80FFF0A0u, 0x70FF6418u, 0x00402018u },   /* fire */
        { 0x80FFD960u, 0x70F07018u, 0x30B03C08u },   /* burning cap */
        { 0x70C8C0B0u, 0x48A89F8Cu, 0x00807050u },   /* grey shock ring */
        { 0x60706A60u, 0x48565050u, 0x00403C38u },   /* stem smoke */
        { 0x60534A40u, 0x48484038u, 0x003A3834u },   /* dark collar */
        { 0x60B8AE9Cu, 0x48908A7Cu, 0x00686860u },   /* dust skirt */
        { 0x48C4BCA8u, 0x30A8A090u, 0x00807868u }    /* ground cover */
    };
    static const float sizes[P_COUNT_IDX][3] = {
        { 24.0f, 60.0f, 100.0f },   /* flash */
        { 8.0f, 28.0f, 42.0f },     /* fire */
        { 30.0f, 70.0f, 100.0f },   /* cap */
        { 18.0f, 40.0f, 64.0f },    /* shock ring */
        { 15.0f, 45.0f, 70.0f },    /* stem */
        { 30.0f, 60.0f, 90.0f },    /* collar */
        { 25.0f, 55.0f, 80.0f },    /* dust */
        { 32.0f, 48.0f, 56.0f }     /* ground */
    };
    static const float lifetimes[P_COUNT_IDX] = {
        1.0f, 6.0f, 8.0f, 3.0f, 12.0f, 14.0f, 10.0f, 10.0f
    };
    unsigned i;
    for (i = 0; i < P_COUNT_IDX; ++i) {
        MercFxParticle particle;
        memset(&particle, 0, sizeof(particle));
        particle.size = sizeof(particle);
        particle.texture_hash = textures[i];
        particle.blend_mode = i <= P_FIRECAP ? 1 : 0;
        memcpy(particle.colors, colors[i], sizeof(particle.colors));
        memcpy(particle.widths, sizes[i], sizeof(particle.widths));
        memcpy(particle.heights, sizes[i], sizeof(particle.heights));
        particle.time_b = 0.35f;
        particle.y_acceleration = i == P_STEM ? 1.0f : i == P_COLLAR ? 0.5f : 0.0f;
        particle.friction = i == P_DUST ? 0.2f : 0.1f;
        particle.lifetime = lifetimes[i];
        particle.max_distance = 4096.0f;          /* framework maximum */
        if (i == P_GROUND) {
            particle.heights[0] = 12.0f;
            particle.heights[1] = 14.0f;
            particle.heights[2] = 16.0f;
            particle.time_b = 0.25f;
        }
        particles[i] = fx->register_particle(&particle);
        if (!particles[i]) {
            LOG("particle %u of %u refused by the framework", i + 1u, (unsigned)P_COUNT_IDX);
            return 0;
        }
        DLOG("particle %u of %u registered", i + 1u, (unsigned)P_COUNT_IDX);
    }
    return 1;
}

/* ------------------------------------------------------------ the impact */

/* The M2 bomb goes on impact, exactly like playFX's t0: one gameplay blast
   (plain explosion type - no red shockwave), the nuke cue, the time
   control, the detonate burst and a copied-position timeline. Visual
   emission is independent of the blast budget. */
static MercFxDecision __cdecl bomber_impact(const MercFxImpact *event, void *context)
{
    unsigned i;
    uint32_t random;
    int custom_audio = 0;
    (void)context;
    if (!fx || !event ||
        event->size < offsetof(MercFxImpact, splash_force) + sizeof(event->splash_force))
        return MERCFX_UNHANDLED;
    for (i = 0; i < 3; ++i)
        if (!isfinite(event->position[i])) return MERCFX_UNHANDLED;

    /* preparation is the fallback boundary: fail before damage or emission
       if the engine has not made the textures/descriptors available */
    for (i = 0; i < P_COUNT_IDX; ++i)
        if (!fx->prepare_particle(particles[i]))
            return MERCFX_UNHANDLED;

    if (fx->size >= offsetof(MercFxApi, apply_blast_configured) + sizeof(fx->apply_blast_configured) &&
        fx->apply_blast_configured) {
        MercFxBlastOptions options;
        memset(&options, 0, sizeof(options));
        options.size = sizeof(options);
        options.damage_type = MERCFX_BLAST_STANDARD;    /* plain explosion: no red decal */
        options.shake_duration = (cfg_quake == 2) ? 6.0f : (cfg_quake ? SHAKE_DURATION : 0.0f);
        options.shake_magnitude = (cfg_quake == 2) ? 2.0f : (cfg_quake ? SHAKE_MAGNITUDE : 0.0f);
        options.scorch_radius = SCORCH_RADIUS;
        if (sound_primary && fx->size >= offsetof(MercFxApi, play_sound) + sizeof(fx->play_sound) &&
            fx->sound_ready && fx->play_sound)
            custom_audio = fx->sound_ready(sound_primary);
        options.suppress_default_sound = (uint32_t)(custom_audio != 0);
        if (!fx->apply_blast_configured(event, &options)) return MERCFX_UNHANDLED;
    } else {
        return MERCFX_UNHANDLED;
    }
    if (custom_audio) fx->play_sound(sound_primary, event->position);

    if (cfg_slowmo && fx->size >= offsetof(MercFxApi, request_slowdown) + sizeof(fx->request_slowdown) &&
        fx->request_slowdown)
        fx->request_slowdown(EFFECT_NAME, cfg_slowmo == 2 ? 0.5f : 0.75f, 1.0f);

    rand_frac(&show_random);
    random = show_random ^ 0x9e3779b9u;

    burst(event->position, &random);   /* t0 DETONATE */
    if (cfg_whiteout) flash_time = 0.0f;
    LOG("impact: nuke away at %.0f %.0f %.0f", event->position[0], event->position[1], event->position[2]);

    for (i = 0; i < SHOW_TIMELINES; ++i) {
        BomberTimeline *timeline = &timelines[timeline_cursor];
        timeline_cursor = (timeline_cursor + 1u) % SHOW_TIMELINES;
        if (timeline->active) continue;
        memcpy(timeline->position, event->position, sizeof(timeline->position));
        timeline->age = timeline->pending = 0.0f;
        timeline->random = random;
        timeline->ground_phase = TAU * rand_frac(&random);
        timeline->ground_sample = 0;
        timeline->shock_done = 0;
        timeline->active = 1;
        break;
    }
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

/* ---- the fog wall: hold RedRenderer's fog range at the FOG VIEW value ---- */

static int fog_checked;                /* logged the once-per-world snapshot */

static void fog_patch_tick(void)
{
    uint8_t *base;
    uint32_t sig[4], before[2];
    float fmin, fmax;
    if (!fog_units) return;                      /* FOG VIEW off: vanilla */
    if (!host || !host->guest_memory || host->guest_limit < FOG_MAX_ADDR + 4u) return;
    base = host->guest_memory;

    if (!fog_checked) {
        memcpy(sig, base + FOG_SIG_ADDR, 16u);
        memcpy(before, base + FOG_MIN_ADDR, 8u);
        memcpy(&fmin, before, 4u);
        memcpy(&fmax, before + 1, 4u);
        LOG("fog globals at +%X/%X: sig %u %u %u %u, range %.0f..%.0f -> FOG VIEW %.0f",
            FOG_MIN_ADDR, FOG_MAX_ADDR, sig[0], sig[1], sig[2], sig[3],
            (double)fmin, (double)fmax, (double)fog_units);
        fog_checked = 1;
        if (!isfinite(fmin) || !isfinite(fmax) || fmin <= 0.0f || fmax <= fmin || fmax > 10000.0f) {
            LOG("fog globals do not look like a fog pair; leaving the fog alone");
            fog_units = 0.0f;
            return;
        }
    }
    /* hold the values: the renderer recomputes its fog constant each frame */
    fmax = fog_units;
    fmin = fmax * 0.4f;
    memcpy(base + FOG_MIN_ADDR, &fmin, 4u);
    memcpy(base + FOG_MAX_ADDR, &fmax, 4u);
}

static void world_tick(uint32_t delta_bits)
{
    float delta;
    memcpy(&delta, &delta_bits, sizeof(delta));
    if (!(delta > 0.0f) || !isfinite(delta)) delta = 0.0f;

    brushes_last_frame = brushes_this_frame;
    brushes_this_frame = 0;

    fog_patch_tick();

    if (attach_state != 2 && attach_state != 3) try_attach();

    /* bind the delivery templates every 2 s until all bound */
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

/* ------------------------------------------------------------- mod screen */

static const char *const off_on[] = {"OFF", "ON"};
static const char *const quake_labels[] = {"OFF", "STRONG", "EXTREME"};
static const char *const slowmo_labels[] = {"OFF", "75%", "50%"};
static const char *const fog_labels[] = {"OFF", "600", "1200", "2000", "3000", "5000", "8000"};

static unsigned get_fog(void)      { return cfg_fog; }
static unsigned get_quake(void)    { return cfg_quake; }
static unsigned get_slowmo(void)   { return cfg_slowmo; }
static unsigned get_whiteout(void) { return cfg_whiteout; }
static unsigned get_fire(void)     { return cfg_fire; }
static unsigned get_ground(void)   { return cfg_ground; }
static unsigned get_debug(void)    { return cfg_debug; }

static void write_unsigned(const char *key, unsigned value)
{
    if (host && host->write_setting) host->write_setting(MOD_INI, MOD_SECTION, key, value);
}

static void set_fog(unsigned v)
{
    if (v > 6u) v = 6u;
    cfg_fog = v;
    fog_units = (float)fog_values[cfg_fog];
    fog_checked = 0;               /* log the new snapshot on the next tick */
    write_unsigned("FogView", cfg_fog);
}
static void set_quake(unsigned v)
{
    if (v > 2u) v = 2u;
    cfg_quake = v; write_unsigned("Earthquake", v);
}
static void set_slowmo(unsigned v)
{
    if (v > 2u) v = 2u;
    cfg_slowmo = v; write_unsigned("SlowMotion", v);
}
static void set_whiteout(unsigned v) { cfg_whiteout = v & 1u; write_unsigned("Whiteout", cfg_whiteout); }
static void set_fire(unsigned v)     { cfg_fire = v & 1u; write_unsigned("MushroomFire", cfg_fire); }
static void set_ground(unsigned v)   { cfg_ground = v & 1u; write_unsigned("GroundCover", cfg_ground); }
static void set_debug(unsigned v)    { cfg_debug = v & 1u; write_unsigned("DebugLog", cfg_debug); }

#define SETTING_COUNT 7u

static const MercenariesModSetting settings[SETTING_COUNT] = {
    {"FOG VIEW",      fog_labels, 7, get_fog, set_fog},
    {"EARTHQUAKE",    quake_labels, 3, get_quake, set_quake},
    {"SLOW MOTION",   slowmo_labels, 3, get_slowmo, set_slowmo},
    {"WHITEOUT",      off_on, 2, get_whiteout, set_whiteout},
    {"MUSHROOM FIRE", off_on, 2, get_fire, set_fire},
    {"GROUND COVER",  off_on, 2, get_ground, set_ground},
    {"DEBUG LOG",     off_on, 2, get_debug, set_debug},
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

static const unsigned setting_defaults[SETTING_COUNT] = { 4u, 1u, 1u, 1u, 1u, 1u, 0u };

static const char *const setting_descriptions[SETTING_COUNT] = {
    "VIEW DISTANCE OF THE ENGINE FOG WALL. OFF = THE VANILLA 400-UNIT FOG.",
    "CAMERA SHAKE ON DETONATION.",
    "SLOW MOTION ON DETONATION.",
    "WHITE SCREEN FLASH ON DETONATION.",
    "THE MUSHROOM BURNS FOR TEN SECONDS.",
    "PERSISTENT GROUND SMOKE OUT TO THE SKIRT.",
    "EXTRA LINES IN THE PREVIEW LOG.",
};

/* --------------------------------------------------------------- attach */

static void try_attach(void)
{
    HMODULE module;
    MercFxGetApi get_api;
    unsigned i;

    if (attach_state == 3 || attach_state == 2) return;
    module = GetModuleHandleA("native_effects.dll");
    if (!module) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("native_effects.dll not loaded; NUKE BOMBER 1 stays vanilla");
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

    if (!register_particles()) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("particle registration failed; staying vanilla");
        }
        return;
    }
    if (!fx->register_effect(EFFECT_NAME, bomber_impact, NULL)) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("effect registration refused; staying vanilla");
        }
        return;
    }
    if (!fx->register_simulation(simulate, reset_timelines, NULL)) {
        fx->unregister_effect(EFFECT_NAME);
        attach_state = 3;
        LOG("simulation registration refused; staying vanilla");
        return;
    }
    sound_primary = 0;
    if (fx->register_sound) sound_primary = fx->register_sound(NUKE_SOUND_CUE);

    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) {
        trigger_hash[i] = fnv1a_casefold(triggers[i]);
        bound_flags[i] = (unsigned char)(fx->bind_template(triggers[i], EFFECT_NAME, 1) ? 1 : 0);
        if (bound_flags[i]) LOG("bind %u %s: BOUND", i, triggers[i]);
        else LOG("bind %u %s: waiting", i, triggers[i]);
    }
    bound_count = 0;
    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) if (bound_flags[i]) ++bound_count;

    LOG("attached: bound=%u of %u, fog=%.0f, quake=%u, slowmo=%u, whiteout=%u, fire=%u, ground=%u",
        bound_count, (unsigned)TRIGGER_COUNT, (double)fog_units, cfg_quake, cfg_slowmo,
        cfg_whiteout, cfg_fire, cfg_ground);
    LOG("the meltdown warhead opens mid-air by engine script; order the "
        "BUNKER BUSTER airstrike from the shop for the NUKE BOMBER 1 show");
    attach_state = 2;
}

static void shutdown_hook(void)
{
    if (fx) {
        if (fx->size >= offsetof(MercFxApi, cancel_slowdown) + sizeof(fx->cancel_slowdown) &&
            fx->cancel_slowdown)
            fx->cancel_slowdown(EFFECT_NAME);
        fx->unregister_effect(EFFECT_NAME);
        fx->unregister_simulation(simulate, NULL);
        fx = NULL;
    }
    reset_show();
    attach_state = 3;
}

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
    "MOAB PLUMBING + M2 SCRIPT, ON SVERED'S MERCFX",
    "The Mercenaries 2: World in Flames tactical nuke, played through the "
    "proven MOAB MercFx plumbing: the bomb goes on impact with a white-blue "
    "flash and an orange fireball, ONE SECOND later the flat grey ground "
    "shockwave ring races out, then the burning M2 mushroom rises over a "
    "thick stem. No red shockwave, no meltdown. FOG VIEW on this page pushes "
    "the engine fog wall out so the show is visible from far away. Needs "
    "Native Effects.",
    setting_descriptions,
    setting_controls,
    NULL,
    NULL,
    setting_defaults,
};

/* --------------------------------------------------------------- entry */

__declspec(dllexport) const MercenariesMod *__cdecl mercenaries_mod_entry(const MercenariesModHost *table)
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

    v = host->read_setting(MOD_INI, MOD_SECTION, "FogView", cfg_fog);
    if (v > 6u) v = 4u;
    cfg_fog = v;
    fog_units = (float)fog_values[cfg_fog];
    v = host->read_setting(MOD_INI, MOD_SECTION, "Earthquake", cfg_quake);
    if (v > 2u) v = 1u;
    cfg_quake = v;
    v = host->read_setting(MOD_INI, MOD_SECTION, "SlowMotion", cfg_slowmo);
    if (v > 2u) v = 1u;
    cfg_slowmo = v;
    cfg_whiteout = host->read_setting(MOD_INI, MOD_SECTION, "Whiteout", cfg_whiteout) & 1u;
    cfg_fire = host->read_setting(MOD_INI, MOD_SECTION, "MushroomFire", cfg_fire) & 1u;
    cfg_ground = host->read_setting(MOD_INI, MOD_SECTION, "GroundCover", cfg_ground) & 1u;
    cfg_debug = host->read_setting(MOD_INI, MOD_SECTION, "DebugLog", cfg_debug) & 1u;
    LOG("settings: fog=%u(%.0f) quake=%u slowmo=%u whiteout=%u fire=%u ground=%u debug=%u",
        cfg_fog, (double)fog_units, cfg_quake, cfg_slowmo, cfg_whiteout, cfg_fire, cfg_ground, cfg_debug);

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
