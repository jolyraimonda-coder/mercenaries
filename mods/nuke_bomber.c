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
#define MOD_VERSION         "1.2.0"
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
};
#define TRIGGER_COUNT (sizeof(triggers) / sizeof(triggers[0]))

/* the ICBM delivery's projectile templates, bound to the icbm_main effect */
static const char *const icbm_triggers[] = {
    "template_amm_cruisemissile",
    "template_amm_cruisemissile0",
    "template_amm_fuelairbomb",
    "template_amm_fuelairbomb0",
    "template_amm_meltdown",
};
#define ICBM_TRIGGER_COUNT (sizeof(icbm_triggers) / sizeof(icbm_triggers[0]))

/* Retail sprite helpers for the whiteout flash (verified addresses). */
#define SPRITE_BEGIN_FN     0x0020A0D0u
#define SPRITE_QUAD_FN      0x0020C980u
#define EXPLOSION_FN        0x000B6460u   /* retail RsExplosion detonation */

/* Retail shop addresses (Mod Manager documentation), for stocking the ICBM. */
#define SHOP_OBJECT         0x0034ABC8u
#define SHOP_STOCKED_FN     0x000C8BF0u
#define SHOP_STOCK_FN       0x000CDA60u

/* The ICBM NUKE STRIKE: a new airstrike on the shop's AIR STRIKES tab. Its
 * delivery is the retail cruise missile support, whose projectile this mod
 * intercepts and turns into the silo launch sequence. */
#define ICBM_SHOP_TEMPLATE  "template_support_p_cruiseMissile"
#define ICBM_SHOP_TEXTURE   "shop_bunkerbuster"
#define ICBM_NAME_KEY       "airstrikes.icbm_nuke_strike"
#define ICBM_SHORT_KEY      "airstrikes.icbm_nuke_strike.short"
#define ICBM_LONG_KEY       "airstrikes.icbm_nuke_strike.long"
#define ICBM_PRICE          150000u

/* The silo launch cues (RsActorBuildingSilo.cpp, LaunchMissile). */
#define ICBM_LAUNCH_CUE     "base.nuke_launch_exp"
#define ICBM_FLIGHT_CUE     "base.nuke_launch_lp"

/* The M2 show, one size: the story nuke of pmccon004, scaled to the
   towering World in Flames cloud. */
#define SHOW_TIMELINES      8
#define SHOW_DURATION       55.0f         /* seconds of cloud */
#define SHOCK_DELAY         1.0f          /* the Lua's after(1.0, ...) */
#define SHOCK_SPEED         75.0f         /* flat grey ring, ground level */
#define FIREBALL_TIME       4.0f          /* the opening fireball burns out */
#define CAP_TOP_TIME        6.0f          /* the cap is up by six seconds */
#define CAP_HEIGHT          130.0f        /* the towering cloud */
#define CAP_RADIUS          95.0f         /* sprite-size cap: multiple sprites */
#define FIRE_TIME           15.0f         /* the whole cloud BURNS this long */
#define GLOW_TIME           20.0f         /* orange glow lingers in the column */
#define FOOT_FIRE_TIME      8.0f          /* flames at the cap foot */
#define FLAME_END           11.0f         /* ground flames burn this long */
#define FLAME_RADIUS        90.0f         /* the burning ground's radius */
#define DUST_CAP            300.0f        /* the skirt's widest radius */
#define FLASH_HOLD          1.0f          /* white screen, like the M2 flash */
#define FLASH_FADE          3.0f
#define FLASH_PASSES        3u            /* stacked quads for a solid white */

/* The ICBM sequence, staged like the engine's own silo
   (RsActorBuildingSilo.cpp: gates and steam, missile rising, launch). */
#define ICBM_STEAM_TIME     2.2f          /* gates open, steam builds */
#define ICBM_ASCENT_TIME    3.0f          /* the missile climbs away */
#define ICBM_FLIGHT_TIME    2.5f          /* it arcs over to the target */
#define ICBM_DESCENT_TIME   1.5f          /* it comes down on the target */
#define ICBM_HOLD_TIME      2.5f          /* the camera holds on the blast */
#define ICBM_SILO_DIST      280.0f        /* the launch silo's distance */
#define ICBM_APEX           340.0f        /* the arc's height over the ground */

/* Explosion cue and time control (the MOAB values, they fit the nuke). */
#define NUKE_SOUND_CUE      "a_strike.bb_explosion"
#define SHAKE_DURATION      4.0f          /* raw engine duration */
#define SHAKE_MAGNITUDE     1.0f          /* retail bunker-buster peak */
#define SCORCH_RADIUS       40.0f         /* framework maximum */

/* The fog wall (see the header comment): guest addresses of RedRenderer's
 * _fFogMin/_fFogMax, CONFIRMED AT RUNTIME (bridge log 15:15: the pair found
 * at +0x30F278/0x30F27C with the vanilla 200/400 values; the fog color sits
 * at +0x30F280 and the game sets it per map). */
#define FOG_MIN_ADDR        0x30F278u
#define FOG_MAX_ADDR        0x30F27Cu

/* ------------------------------------------------------------- tiny helpers */

#define TAU 6.2831853f

static const MercenariesModHost *host;
static const MercFxApi *fx;
static unsigned attach_state;          /* 0 idle, 1 waiting, 2 live, 3 vanilla */
static unsigned attach_attempts;

static uint32_t trigger_hash[TRIGGER_COUNT];
static unsigned char bound_flags[TRIGGER_COUNT];
static unsigned bound_count;

static uint32_t particles[10];
enum { P_FLASH, P_FIRE, P_FIRECAP, P_SHOCK, P_STEM, P_COLLAR, P_DUST, P_GROUND,
       P_FLAME, P_STEAM, P_COUNT_IDX };
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
static unsigned cfg_flames = 1;        /* GROUND FLAMES */
static unsigned cfg_cinema = 1;        /* CINEMATIC CAMERA */
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
static void icbm_step(float dt);
static void icbm_visuals(void);
static void camera_hook(uint32_t camera, uint32_t matrix, uint32_t delta_bits);

#define LOG(...) do { if (host && host->log) host->log("nuke_bomber", __VA_ARGS__); } while (0)
#define DLOG(...) do { if (cfg_debug && host && host->log) host->log("nuke_bomber_debug", __VA_ARGS__); } while (0)

/* --------------------------------------------------------------- the show */

typedef struct BomberTimeline {
    float position[3];
    float age, pending;          /* elapsed simulation time, emission remainder */
    uint32_t random, ground_sample, flame_sample;
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

/* t0 DETONATE, stages 1+2: the white-yellow nuclear flash, then the yellow
   fireball sphere expanding under golden-orange fire. */
static void burst(const float origin[3], uint32_t *random)
{
    unsigned i;
    float position[3] = { origin[0], origin[1] + 2.0f, origin[2] };
    emit2(P_FLASH, position, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    for (i = 0; i < 30; ++i) {                    /* the yellow fireball sphere */
        float angle = TAU * rand_frac(random);
        float speed = 6.0f + 14.0f * rand_frac(random);
        emit_rand(P_FIRE, position, cosf(angle) * speed,
                  2.0f + 6.0f * rand_frac(random), sinf(angle) * speed, random);
    }
    for (i = 0; i < 36; ++i) {                    /* golden-orange fire around it */
        float angle = TAU * rand_frac(random);
        float speed = 10.0f + 18.0f * rand_frac(random);
        emit_rand(P_FIRECAP, position, cosf(angle) * speed,
                  1.0f + 4.0f * rand_frac(random), sinf(angle) * speed, random);
    }
    for (i = 0; i < 16; ++i) {                    /* the rising fire column start */
        float angle = TAU * rand_frac(random);
        float speed = 4.0f + 5.0f * rand_frac(random);
        emit_rand(P_FIRECAP, position, cosf(angle) * speed,
                  5.0f + 6.0f * rand_frac(random), sinf(angle) * speed, random);
    }
    for (i = 0; i < 20; ++i) {                    /* dark dust dome under it all */
        float angle = TAU * (float)i / 20.0f;
        float x = cosf(angle), z = sinf(angle);
        position[0] = origin[0] + x * 5.0f;
        position[1] = origin[1] + 0.75f;
        position[2] = origin[2] + z * 5.0f;
        emit_rand(P_STEM, position, x * 26.0f, 0.5f, z * 26.0f, random);
    }
}

/* t+1.0 SHOCKWAVE, stage 5: expanding ground haze - warm dust with an
   orange atmospheric tint, hugging the terrain, racing outward. */
static void shockwave(const float origin[3], uint32_t *random)
{
    unsigned i;
    for (i = 0; i < 24; ++i) {
        float angle = TAU * (float)i / 24.0f;
        float x = cosf(angle), z = sinf(angle);
        float position[3] = { origin[0] + x * 6.0f, origin[1] + 1.5f, origin[2] + z * 6.0f };
        emit_rand(P_SHOCK, position, x * SHOCK_SPEED, 0.0f, z * SHOCK_SPEED, random);
    }
    for (i = 0; i < 12; ++i) {                    /* warm dust backing the haze */
        float angle = TAU * rand_frac(random);
        float speed = 20.0f + 15.0f * rand_frac(random);
        float position[3] = { origin[0], origin[1] + 1.0f, origin[2] };
        emit_rand(P_DUST, position, cosf(angle) * speed, 0.3f,
                  sinf(angle) * speed, random);
    }
}

/* the mushroom deck, stage 3: BRIGHT YELLOW top over a dark underside -
   yellow fireball sprites crown the cap, burning-orange fire spills
   around it, dark collar smoke hangs underneath. */
static void cap_deck(const BomberTimeline *timeline, float height, float radius,
                     float fire, uint32_t *random)
{
    unsigned i;
    for (i = 0; i < 3; ++i) {                     /* bright yellow top */
        float angle = TAU * rand_frac(random);
        float spread = radius * (0.3f + 0.5f * rand_frac(random));
        float x = cosf(angle), z = sinf(angle);
        float position[3] = {
            timeline->position[0] + x * spread,
            timeline->position[1] + height + rand_frac(random) * 8.0f,
            timeline->position[2] + z * spread
        };
        emit_rand(P_FIRE, position, x * 1.0f, 1.0f + 1.5f * rand_frac(random),
                  z * 1.0f, random);
    }
    for (i = 0; i < 3; ++i) {                     /* burning orange around it */
        float angle = TAU * rand_frac(random);
        float spread = radius * (0.6f + 0.4f * rand_frac(random));
        float x = cosf(angle), z = sinf(angle);
        float position[3] = {
            timeline->position[0] + x * spread,
            timeline->position[1] + height - 4.0f + rand_frac(random) * 10.0f,
            timeline->position[2] + z * spread
        };
        unsigned kind = fire > 0.0f ? P_FIRECAP : P_COLLAR;
        emit_rand(kind, position, x * 1.5f, 1.0f + 1.5f * rand_frac(random),
                  z * 1.5f, random);
    }
    for (i = 0; i < 2; ++i) {                     /* the dark underside */
        float angle = TAU * rand_frac(random);
        float spread = radius * (0.7f + 0.3f * rand_frac(random));
        float x = cosf(angle), z = sinf(angle);
        float position[3] = {
            timeline->position[0] + x * spread,
            timeline->position[1] + height - 10.0f - rand_frac(random) * 6.0f,
            timeline->position[2] + z * spread
        };
        emit_rand(P_COLLAR, position, x * 1.2f, 0.4f, z * 1.2f, random);
    }
}

/* the burning ground: colored flames spread over the terrain around the
   blast, burn high, then fade away - stage 6's lingering fire. */
static void emit_flames(BomberTimeline *timeline)
{
    unsigned i;
    if (timeline->age < SHOCK_DELAY || timeline->age >= FLAME_END) return;
    if (fx->size < offsetof(MercFxApi, sample_terrain_height) + sizeof(fx->sample_terrain_height) ||
        !fx->sample_terrain_height)
        return;
    for (i = 0; i < 5u; ++i) {
        uint32_t sample = timeline->flame_sample++;
        float radius = FLAME_RADIUS * sqrtf(fmodf(0.3f + (float)sample * 0.61803398f, 1.0f));
        float angle = (float)sample * 2.39996322973f;
        float position[3] = {
            timeline->position[0] + cosf(angle) * radius, 0.0f,
            timeline->position[2] + sinf(angle) * radius
        };
        float height;
        if (!fx->sample_terrain_height(position[0], position[2], &height) ||
            !isfinite(height) || !isfinite(height + 1.0f))
            continue;
        position[1] = height + 1.0f;
        emit2(P_FLAME, position,
              (rand_frac(&timeline->random) - 0.5f) * 2.0f,
              3.0f + 5.0f * rand_frac(&timeline->random),
              (rand_frac(&timeline->random) - 0.5f) * 2.0f,
              0.0f, 1.0f);
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
    icbm_visuals();
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
        if (cfg_flames) emit_flames(timeline);

        /* stage 2: the yellow fireball sphere expands 0..4 s */
        if (timeline->age < FIREBALL_TIME) {
            for (j = 0; j < 4; ++j) {
                float angle = TAU * rand_frac(&timeline->random);
                float x = cosf(angle), z = sinf(angle);
                float radius = timeline->age * 10.0f;
                float position[3] = {
                    timeline->position[0] + x * radius,
                    timeline->position[1] + 2.0f + timeline->age * 4.0f,
                    timeline->position[2] + z * radius
                };
                emit_rand(P_FIRE, position, x * 2.0f, 5.0f, z * 2.0f, &timeline->random);
            }
        }

        /* stage 3: the mushroom - bright yellow cap rising 0..6 s over a
           dark underside, the whole cloud burning to 15 s */
        if (cfg_fire && timeline->age < FIRE_TIME) {
            float up = timeline->age / CAP_TOP_TIME;
            if (up > 1.0f) up = 1.0f;
            cap_deck(timeline, 8.0f + up * CAP_HEIGHT,
                     20.0f + up * CAP_RADIUS,
                     timeline->age < FIRE_TIME ? 1.0f : 0.0f, &timeline->random);
            if (timeline->age < FOOT_FIRE_TIME) {   /* flames at the cap foot */
                for (j = 0; j < 2; ++j) {
                    float angle = TAU * rand_frac(&timeline->random);
                    float x = cosf(angle), z = sinf(angle);
                    float position[3] = {
                        timeline->position[0] + x * 8.0f,
                        timeline->position[1] + 2.0f,
                        timeline->position[2] + z * 8.0f
                    };
                    emit_rand(P_FIRECAP, position, x * 0.5f, 3.0f, z * 0.5f,
                              &timeline->random);
                }
            }
        }

        /* stage 4: the smoke column - dark brown-black with orange glow
           inside it while it is young */
        if (timeline->age >= 1.5f && timeline->age < 30.0f) {
            for (j = 0; j < 2; ++j) {
                float angle = TAU * rand_frac(&timeline->random);
                float spread = 1.0f + 3.0f * rand_frac(&timeline->random);
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
            if (timeline->age < GLOW_TIME) {        /* the orange glow */
                float angle = TAU * rand_frac(&timeline->random);
                float x = cosf(angle), z = sinf(angle);
                float position[3] = {
                    timeline->position[0] + x * 2.0f,
                    timeline->position[1] + 6.0f + rand_frac(&timeline->random) * (CAP_HEIGHT - 20.0f),
                    timeline->position[2] + z * 2.0f
                };
                emit_rand(P_FIRECAP, position, x * 0.2f, 0.8f, z * 0.2f, &timeline->random);
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
        { 0x80E8EDAFu, 0x60E4E257u, 0x00D0CC70u },   /* white-yellow nuclear flash */
        { 0x80E4E257u, 0x70D8C840u, 0x30A08020u },   /* main yellow fireball */
        { 0x80DDAC4Bu, 0x70B47D48u, 0x30705230u },   /* golden to burning orange fire */
        { 0x60C8B890u, 0x48C09858u, 0x00807050u },   /* warm ground-haze shockwave */
        { 0x70473D2Du, 0x50453726u, 0x00393124u },   /* dark column smoke */
        { 0x70473D2Du, 0x48453A28u, 0x00393124u },   /* dark cap underside */
        { 0x60A89070u, 0x48806850u, 0x00504838u },   /* brown-grey skirt and aftermath */
        { 0x48B8A888u, 0x30A09880u, 0x00706858u },   /* terrain ground cover */
        { 0x80FFF040u, 0x70FF8020u, 0x30C03010u },   /* colored ground flame */
        { 0x60F0F0EAu, 0x48D8E0E0u, 0x00A8C0C0u }    /* white launch steam */
    };
    static const float sizes[P_COUNT_IDX][3] = {
        { 24.0f, 70.0f, 100.0f },   /* flash */
        { 10.0f, 30.0f, 45.0f },    /* yellow fireball */
        { 25.0f, 60.0f, 100.0f },   /* burning orange */
        { 20.0f, 45.0f, 70.0f },    /* ground haze ring */
        { 20.0f, 50.0f, 80.0f },    /* column */
        { 35.0f, 65.0f, 95.0f },    /* collar */
        { 25.0f, 55.0f, 85.0f },    /* dust */
        { 32.0f, 48.0f, 56.0f },    /* ground */
        { 5.0f, 14.0f, 24.0f },     /* flame */
        { 8.0f, 20.0f, 36.0f }      /* steam */
    };
    static const float lifetimes[P_COUNT_IDX] = {
        1.0f, 5.0f, 8.0f, 3.0f, 12.0f, 14.0f, 10.0f, 10.0f, 2.0f, 1.6f
    };
    unsigned i;
    for (i = 0; i < P_COUNT_IDX; ++i) {
        MercFxParticle particle;
        memset(&particle, 0, sizeof(particle));
        particle.size = sizeof(particle);
        particle.texture_hash = textures[i];
        particle.blend_mode = i <= P_FIRECAP || i == P_FLAME ? 1 : 0;
        memcpy(particle.colors, colors[i], sizeof(particle.colors));
        memcpy(particle.widths, sizes[i], sizeof(particle.widths));
        memcpy(particle.heights, sizes[i], sizeof(particle.heights));
        particle.time_b = 0.35f;
        particle.y_acceleration = i == P_STEM ? 1.0f : i == P_COLLAR ? 0.5f :
                                  i == P_FLAME ? 2.5f : i == P_STEAM ? 1.0f : 0.0f;
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

/* ------------------------------------------------------- the ICBM strike */

/* The ICBM NUKE STRIKE: ordering it delivers the cruise missile, whose
   projectile this effect intercepts WITHOUT detonating; instead the mod
   stages the launch the engine's own silo performs
   (RsActorBuildingSilo.cpp: gates and steam, the missile rising, launch
   cue base.nuke_launch_exp), flies the missile over to the ordered point,
   and detonates the nuke there by the retail explosion. The camera hook
   makes it a scene: it watches the silo, follows the missile and holds on
   the blast, then hands the camera back. */

enum { ICBM_IDLE, ICBM_STEAM, ICBM_ASCENT, ICBM_FLIGHT, ICBM_DESCENT,
       ICBM_BLAST, ICBM_HOLD, ICBM_COUNT };
static const char *const icbm_phase_names[ICBM_COUNT] = {
    "idle", "steam", "ascent", "flight", "descent", "blast", "hold"
};

static int icbm_phase;
static float icbm_t;                       /* seconds inside the phase */
static float icbm_target[3], icbm_silo[3], icbm_nose[3], icbm_fly_start[3];
static float icbm_speed;
static float icbm_saved[16];               /* the camera's rows before the scene */
static int icbm_cam_saved;
static uint32_t sound_launch, sound_flight;

static void icbm_set_phase(int phase)
{
    icbm_phase = phase;
    icbm_t = 0.0f;
    DLOG("icbm phase: %s", icbm_phase_names[phase]);
}

/* intercept: no blast here - the sequence detonates later */
static MercFxDecision __cdecl icbm_impact(const MercFxImpact *event, void *context)
{
    unsigned i;
    float height, angle;
    (void)context;
    if (!fx || !event || icbm_phase != ICBM_IDLE)
        return MERCFX_REMOVE;              /* a scene is already playing */
    if (!cfg_cinema)                        /* no camera scene: detonate at once */
        return bomber_impact(event, context);
    if (event->size < offsetof(MercFxImpact, splash_force) + sizeof(event->splash_force))
        return MERCFX_REMOVE;
    for (i = 0; i < 3; ++i)
        if (!isfinite(event->position[i])) return MERCFX_REMOVE;

    memcpy(icbm_target, event->position, sizeof(icbm_target));
    angle = 0.7f;
    icbm_silo[0] = icbm_target[0] + cosf(angle) * ICBM_SILO_DIST;
    icbm_silo[2] = icbm_target[2] + sinf(angle) * ICBM_SILO_DIST;
    icbm_silo[1] = icbm_target[1];
    if (fx->sample_terrain_height &&
        fx->sample_terrain_height(icbm_silo[0], icbm_silo[2], &height) && isfinite(height))
        icbm_silo[1] = height;
    memcpy(icbm_nose, icbm_silo, sizeof(icbm_nose));
    icbm_speed = 0.0f;
    icbm_cam_saved = 0;
    icbm_set_phase(ICBM_STEAM);
    LOG("icbm away: silo at %.0f %.0f %.0f, target at %.0f %.0f %.0f",
        icbm_silo[0], icbm_silo[1], icbm_silo[2],
        icbm_target[0], icbm_target[1], icbm_target[2]);
    return MERCFX_REMOVE;
}

/* the sequence's clock and its detonation; runs in world_tick */
static void icbm_step(float dt)
{
    float height;
    if (icbm_phase == ICBM_IDLE || !fx) return;
    icbm_t += dt;
    switch (icbm_phase) {
    case ICBM_STEAM:
        if (icbm_t >= ICBM_STEAM_TIME) {
            if (sound_launch && fx->sound_ready && fx->sound_ready(sound_launch))
                fx->play_sound(sound_launch, icbm_silo);
            icbm_set_phase(ICBM_ASCENT);
        }
        break;
    case ICBM_ASCENT:
        icbm_speed += 30.0f * dt;
        icbm_nose[1] += icbm_speed * dt;
        icbm_nose[0] += (icbm_target[0] - icbm_silo[0]) * 0.06f * dt;
        icbm_nose[2] += (icbm_target[2] - icbm_silo[2]) * 0.06f * dt;
        if (icbm_t >= ICBM_ASCENT_TIME) {
            memcpy(icbm_fly_start, icbm_nose, sizeof(icbm_fly_start));
            icbm_set_phase(ICBM_FLIGHT);
        }
        break;
    case ICBM_FLIGHT: {
        float u = icbm_t / ICBM_FLIGHT_TIME;
        float apex[3];
        if (u > 1.0f) u = 1.0f;
        apex[0] = (icbm_fly_start[0] + icbm_target[0]) * 0.5f;
        apex[2] = (icbm_fly_start[2] + icbm_target[2]) * 0.5f;
        apex[1] = icbm_fly_start[1] + ICBM_APEX;
        /* quadratic bezier: start, apex over the middle, high over the target */
        icbm_nose[0] = (1.0f - u) * (1.0f - u) * icbm_fly_start[0] +
                       2.0f * (1.0f - u) * u * apex[0] + u * u * icbm_target[0];
        icbm_nose[2] = (1.0f - u) * (1.0f - u) * icbm_fly_start[2] +
                       2.0f * (1.0f - u) * u * apex[2] + u * u * icbm_target[2];
        icbm_nose[1] = (1.0f - u) * (1.0f - u) * icbm_fly_start[1] +
                       2.0f * (1.0f - u) * u * apex[1] + u * u * (icbm_target[1] + ICBM_APEX * 0.8f);
        if (icbm_t >= ICBM_FLIGHT_TIME) icbm_set_phase(ICBM_DESCENT);
        break;
    }
    case ICBM_DESCENT:
        icbm_nose[0] += (icbm_target[0] - icbm_nose[0]) * 3.0f * dt;
        icbm_nose[2] += (icbm_target[2] - icbm_nose[2]) * 3.0f * dt;
        icbm_nose[1] -= (icbm_nose[1] - icbm_target[1]) * 3.0f * dt;
        if (icbm_t >= ICBM_DESCENT_TIME) {
            uint32_t args[13];
            BomberTimeline *timeline;
            unsigned i;
            memcpy(args, icbm_target, 12);
            args[3] = float_bits(800.0f);   /* blast radius */
            args[4] = float_bits(400.0f);   /* force */
            args[5] = 0u;
            args[6] = float_bits(8000.0f);  /* max damage */
            args[7] = 0u;                   /* our show plays */
            args[8] = 0u;                   /* our cue replaces the stock sound */
            args[9] = 0u;                   /* plain explosion: no red shockwave */
            args[10] = 0u;
            args[11] = 1u;
            args[12] = 0u;
            host->guest_call(EXPLOSION_FN, 0u, 13u, args);
            if (sound_primary && fx->sound_ready && fx->sound_ready(sound_primary))
                fx->play_sound(sound_primary, icbm_target);
            if (cfg_slowmo && fx->request_slowdown)
                fx->request_slowdown(EFFECT_NAME, cfg_slowmo == 2 ? 0.5f : 0.75f, 1.0f);
            if (cfg_whiteout) flash_time = 0.0f;
            for (i = 0; i < SHOW_TIMELINES; ++i) {
                timeline = &timelines[timeline_cursor];
                timeline_cursor = (timeline_cursor + 1u) % SHOW_TIMELINES;
                if (timeline->active) continue;
                memcpy(timeline->position, icbm_target, sizeof(timeline->position));
                timeline->age = 0.9f;   /* the shockwave goes at once */
                timeline->pending = 0.0f;
                timeline->random = show_random ^ 0x4943424du;
                timeline->ground_phase = TAU * rand_frac(&timeline->random);
                timeline->ground_sample = timeline->flame_sample = 0;
                timeline->shock_done = 0;
                timeline->active = 1;
                break;
            }
            LOG("icbm detonation at %.0f %.0f %.0f", icbm_target[0], icbm_target[1], icbm_target[2]);
            icbm_set_phase(ICBM_HOLD);
        }
        break;
    case ICBM_HOLD:
        if (icbm_t >= ICBM_HOLD_TIME) {
            (void)height;
            icbm_set_phase(ICBM_IDLE);
        }
        break;
    };
}

/* the sequence's visuals; runs inside the framework's simulation step */
static void icbm_visuals(void)
{
    unsigned i;
    float dt_emit = 0.125f;
    if (icbm_phase == ICBM_IDLE || !fx) return;
    switch (icbm_phase) {
    case ICBM_STEAM:                            /* gates open: white steam */
        for (i = 0; i < 3u; ++i) {
            float angle = TAU * rand_frac(&show_random);
            float spread = 2.0f + 6.0f * rand_frac(&show_random);
            float position[3] = {
                icbm_silo[0] + cosf(angle) * spread,
                icbm_silo[1] + 1.0f,
                icbm_silo[2] + sinf(angle) * spread
            };
            emit2(P_STEAM, position, 0.0f, 3.0f + 4.0f * rand_frac(&show_random), 0.0f,
                  0.0f, 0.6f);
        }
        break;
    case ICBM_ASCENT:                           /* ignition and climb */
        emit2(P_FLAME, icbm_nose, 0.0f, -6.0f, 0.0f, 0.0f, 1.0f);
        emit2(P_FIRE, icbm_nose, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        for (i = 0; i < 2u; ++i)                /* the smoke trail */
            emit2(P_STEM, icbm_nose,
                  (rand_frac(&show_random) - 0.5f) * 2.0f, -2.0f,
                  (rand_frac(&show_random) - 0.5f) * 2.0f, 0.0f, 0.8f);
        for (i = 0; i < 4u; ++i) {              /* the pad's spreading smoke */
            float angle = TAU * rand_frac(&show_random);
            float spread = 4.0f + 30.0f * rand_frac(&show_random);
            float position[3] = {
                icbm_silo[0] + cosf(angle) * spread,
                icbm_silo[1] + 1.5f,
                icbm_silo[2] + sinf(angle) * spread
            };
            emit2(P_STEAM, position, cosf(angle) * 6.0f, 1.0f, sinf(angle) * 6.0f,
                  0.0f, 0.8f);
        }
        break;
    case ICBM_FLIGHT:
    case ICBM_DESCENT:                          /* trail all the way in */
        emit2(P_FLAME, icbm_nose, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        for (i = 0; i < 2u; ++i)
            emit2(P_STEM, icbm_nose,
                  (rand_frac(&show_random) - 0.5f) * 3.0f,
                  (rand_frac(&show_random) - 0.5f) * 3.0f,
                  (rand_frac(&show_random) - 0.5f) * 3.0f, 0.0f, 0.8f);
        break;
    };
    (void)dt_emit;
}

/* the cinematic camera: eye + look-at rows while the scene runs */
static void camera_hook(uint32_t camera, uint32_t matrix, uint32_t delta_bits)
{
    uint8_t *m;
    float eye[3], look[3], dir[3], right[3], up[3], len;
    float saved_w[4];
    (void)camera; (void)delta_bits;
    if (icbm_phase == ICBM_IDLE || !cfg_cinema) return;
    if (!host || !host->guest_memory || host->guest_limit < matrix + 64u) return;
    m = host->guest_memory + matrix;

    if (!icbm_cam_saved) {                      /* remember the player's camera */
        memcpy(icbm_saved, m, 64u);
        icbm_cam_saved = 1;
    }

    switch (icbm_phase) {
    case ICBM_STEAM:
        eye[0] = icbm_silo[0] + 55.0f; eye[1] = icbm_silo[1] + 22.0f; eye[2] = icbm_silo[2] + 55.0f;
        look[0] = icbm_silo[0]; look[1] = icbm_silo[1] + 12.0f; look[2] = icbm_silo[2];
        break;
    case ICBM_ASCENT:
        eye[0] = icbm_nose[0] + 38.0f; eye[1] = icbm_nose[1] - 10.0f; eye[2] = icbm_nose[2] + 38.0f;
        look[0] = icbm_nose[0]; look[1] = icbm_nose[1] + 4.0f; look[2] = icbm_nose[2];
        break;
    case ICBM_FLIGHT:
        eye[0] = icbm_nose[0] + 70.0f; eye[1] = icbm_nose[1] - 25.0f; eye[2] = icbm_nose[2] + 70.0f;
        look[0] = icbm_nose[0]; look[1] = icbm_nose[1]; look[2] = icbm_nose[2];
        break;
    case ICBM_DESCENT:
        eye[0] = icbm_target[0] + 90.0f; eye[1] = icbm_target[1] + 45.0f; eye[2] = icbm_target[2] + 90.0f;
        look[0] = icbm_nose[0]; look[1] = icbm_nose[1]; look[2] = icbm_nose[2];
        break;
    case ICBM_BLAST:
    case ICBM_HOLD:
        eye[0] = icbm_target[0] + 110.0f; eye[1] = icbm_target[1] + 55.0f; eye[2] = icbm_target[2] + 110.0f;
        look[0] = icbm_target[0]; look[1] = icbm_target[1] + 40.0f; look[2] = icbm_target[2];
        break;
    default:
        return;
    }

    dir[0] = look[0] - eye[0]; dir[1] = look[1] - eye[1]; dir[2] = look[2] - eye[2];
    len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1.0f) return;
    dir[0] /= len; dir[1] /= len; dir[2] /= len;
    right[0] = dir[2]; right[1] = 0.0f; right[2] = -dir[0];          /* cross(up, dir) */
    len = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (len < 0.01f) { right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f; }
    else { right[0] /= len; right[2] /= len; }
    up[0] = dir[1] * right[2] - dir[2] * right[1];
    up[1] = dir[2] * right[0] - dir[0] * right[2];
    up[2] = dir[0] * right[1] - dir[1] * right[0];

    memcpy(saved_w, m + 12u, 4u);               /* keep each row's w */
    memcpy(saved_w + 1, m + 28u, 4u);
    memcpy(saved_w + 2, m + 44u, 4u);
    memcpy(saved_w + 3, m + 60u, 4u);
    memcpy(m, right, 12u); memcpy(m + 16, up, 12u); memcpy(m + 32, dir, 12u);
    memcpy(m + 48, eye, 12u);
    memcpy(m + 12, saved_w, 4u); memcpy(m + 28, saved_w + 1, 4u);
    memcpy(m + 44, saved_w + 2, 4u); memcpy(m + 60, saved_w + 3, 4u);

    if (icbm_phase == ICBM_HOLD && icbm_t >= ICBM_HOLD_TIME) {
        memcpy(m, icbm_saved, 64u);             /* hand the camera back */
        icbm_cam_saved = 0;
    }
}

/* ------------------------------------------------------------- shop item */

static const MercenariesShopItem icbm_shop_item = {
    ICBM_SHOP_TEMPLATE,
    ICBM_NAME_KEY,
    ICBM_SHORT_KEY,
    ICBM_LONG_KEY,
    ICBM_SHOP_TEXTURE,
    ICBM_PRICE,
    MERCENARIES_SHOP_AIR_STRIKES,
};

static void register_shop_item(void)
{
    if (!host || !host->add_shop_item) return;
    if (host->size < offsetof(MercenariesModHost, add_shop_item) + sizeof(host->add_shop_item)) return;
    if (host->register_string) {
        host->register_string(ICBM_NAME_KEY, "ICBM NUKE STRIKE");
        host->register_string(ICBM_SHORT_KEY, "A silo-launched ICBM with a cinematic camera.");
        host->register_string(ICBM_LONG_KEY,
            "The silo opens and steams, the ICBM rises on a pillar of fire, "
            "arcs over and comes down on your target while the camera "
            "follows it. The nuke goes with the towering burning mushroom "
            "and the ground burns with colored flames. No red shockwave.");
    }
    if (host->add_shop_item(&icbm_shop_item))
        LOG("shop item ICBM NUKE STRIKE added on %s", ICBM_SHOP_TEMPLATE);
    else
        LOG("shop item refused (already added, or the Mod Manager is too old)");
}

static int stock_checked, have_stocked, have_stock;

static void try_stock_item(void)
{
    uint32_t hash = fnv1a_casefold(ICBM_SHOP_TEMPLATE);
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
    LOG("stocked ICBM NUKE STRIKE in the Merchant of Menace");
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
    uint32_t after[2];
    float fmin, fmax;
    if (!fog_units) return;                      /* FOG VIEW off: vanilla */
    if (!host || !host->guest_memory || host->guest_limit < FOG_MAX_ADDR + 8u) return;
    base = host->guest_memory;

    if (!fog_checked) {
        memcpy(&fmin, base + FOG_MIN_ADDR, 4u);
        memcpy(&fmax, base + FOG_MAX_ADDR, 4u);
        memcpy(after, base + FOG_MAX_ADDR + 4u, 8u);   /* fog color + follower */
        LOG("fog wall at +%X/%X: range %.0f..%.0f, color %08X, next %08X -> FOG VIEW %.0f",
            FOG_MIN_ADDR, FOG_MAX_ADDR, (double)fmin, (double)fmax,
            after[0], after[1], (double)fog_units);
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
    icbm_step(delta);

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
            for (i = 0; i < (unsigned)ICBM_TRIGGER_COUNT; ++i)
                fx->bind_template(icbm_triggers[i], "icbm_main", 1);   /* rebind every pass */
            bound_count = ok;
        }
    }

    /* stock the ICBM item once the shop is up, then every 30 s a few times */
    {
        static float stock_timer;
        static unsigned stock_tries;
        stock_timer += delta;
        if (stock_timer >= 3.0f && stock_tries < 5u) {
            stock_timer = 0.0f;
            ++stock_tries;
            try_stock_item();
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
static unsigned get_flames(void)   { return cfg_flames; }
static unsigned get_cinema(void)   { return cfg_cinema; }
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
static void set_flames(unsigned v)   { cfg_flames = v & 1u; write_unsigned("GroundFlames", cfg_flames); }
static void set_cinema(unsigned v)   { cfg_cinema = v & 1u; write_unsigned("Cinematic", cfg_cinema); }
static void set_debug(unsigned v)    { cfg_debug = v & 1u; write_unsigned("DebugLog", cfg_debug); }

#define SETTING_COUNT 9u

static const MercenariesModSetting settings[SETTING_COUNT] = {
    {"FOG VIEW",      fog_labels, 7, get_fog, set_fog},
    {"EARTHQUAKE",    quake_labels, 3, get_quake, set_quake},
    {"SLOW MOTION",   slowmo_labels, 3, get_slowmo, set_slowmo},
    {"WHITEOUT",      off_on, 2, get_whiteout, set_whiteout},
    {"MUSHROOM FIRE", off_on, 2, get_fire, set_fire},
    {"GROUND COVER",  off_on, 2, get_ground, set_ground},
    {"GROUND FLAMES", off_on, 2, get_flames, set_flames},
    {"CINEMATIC",     off_on, 2, get_cinema, set_cinema},
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

static const unsigned setting_defaults[SETTING_COUNT] = { 4u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 0u };

static const char *const setting_descriptions[SETTING_COUNT] = {
    "VIEW DISTANCE OF THE ENGINE FOG WALL. OFF = THE VANILLA 400-UNIT FOG.",
    "CAMERA SHAKE ON DETONATION.",
    "SLOW MOTION ON DETONATION.",
    "WHITE SCREEN FLASH ON DETONATION.",
    "THE MUSHROOM BURNS FOR FIFTEEN SECONDS.",
    "PERSISTENT GROUND SMOKE OUT TO THE SKIRT.",
    "COLORED FLAMES SPREAD AND BURN ON THE GROUND.",
    "THE ICBM'S LAUNCH, FLIGHT AND BLAST PLAY AS A CAMERA SCENE.",
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

    /* the ICBM effect: intercepts the cruise/fuel-air/meltdown projectiles
       and plays the silo launch sequence instead */
    if (fx->register_effect("icbm_main", icbm_impact, NULL)) {
        unsigned k;
        sound_launch = fx->register_sound ? fx->register_sound(ICBM_LAUNCH_CUE) : 0;
        sound_flight = fx->register_sound ? fx->register_sound(ICBM_FLIGHT_CUE) : 0;
        for (k = 0; k < (unsigned)ICBM_TRIGGER_COUNT; ++k) {
            if (fx->bind_template(icbm_triggers[k], "icbm_main", 1))
                LOG("bind icbm %s: BOUND", icbm_triggers[k]);
            else
                LOG("bind icbm %s: waiting", icbm_triggers[k]);
        }
    } else {
        sound_launch = sound_flight = 0;
        LOG("icbm_main effect registration refused; the ICBM strike stays off");
    }

    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) {
        trigger_hash[i] = fnv1a_casefold(triggers[i]);
        bound_flags[i] = (unsigned char)(fx->bind_template(triggers[i], EFFECT_NAME, 1) ? 1 : 0);
        if (bound_flags[i]) LOG("bind %u %s: BOUND", i, triggers[i]);
        else LOG("bind %u %s: waiting", i, triggers[i]);
    }
    bound_count = 0;
    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) if (bound_flags[i]) ++bound_count;

    LOG("attached: bound=%u of %u, fog=%.0f, quake=%u, slowmo=%u, whiteout=%u, fire=%u, ground=%u, flames=%u, cinema=%u",
        bound_count, (unsigned)TRIGGER_COUNT, (double)fog_units, cfg_quake, cfg_slowmo,
        cfg_whiteout, cfg_fire, cfg_ground, cfg_flames, cfg_cinema);
    LOG("order the BUNKER BUSTER airstrike for the direct nuke, or ICBM NUKE "
        "STRIKE for the silo launch with the cinematic camera");
    attach_state = 2;
}

static void shutdown_hook(void)
{
    if (fx) {
        if (fx->size >= offsetof(MercFxApi, cancel_slowdown) + sizeof(fx->cancel_slowdown) &&
            fx->cancel_slowdown)
            fx->cancel_slowdown(EFFECT_NAME);
        fx->unregister_effect(EFFECT_NAME);
        fx->unregister_effect("icbm_main");
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
    "MOAB PLUMBING + M2 SCRIPT + ICBM SILO LAUNCH, ON SVERED'S MERCFX",
    "The Mercenaries 2 tactical nuke through the proven MOAB MercFx plumbing: "
    "the bomb goes on impact with a white-yellow flash and a yellow "
    "fireball, ONE SECOND later the warm ground haze races out, then the "
    "towering burning mushroom rises over a dark stem with colored flames "
    "spreading on the ground. NEW: the ICBM NUKE STRIKE airstrike launches "
    "from a steaming silo and flies to your target in a cinematic camera "
    "scene. No red shockwave, no meltdown. FOG VIEW pushes the engine fog "
    "wall out. Needs Native Effects.",
    setting_descriptions,
    setting_controls,
    camera_hook,
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
    cfg_flames = host->read_setting(MOD_INI, MOD_SECTION, "GroundFlames", cfg_flames) & 1u;
    cfg_cinema = host->read_setting(MOD_INI, MOD_SECTION, "Cinematic", cfg_cinema) & 1u;
    cfg_debug = host->read_setting(MOD_INI, MOD_SECTION, "DebugLog", cfg_debug) & 1u;
    LOG("settings: fog=%u(%.0f) quake=%u slowmo=%u whiteout=%u fire=%u ground=%u flames=%u cinema=%u debug=%u",
        cfg_fog, (double)fog_units, cfg_quake, cfg_slowmo, cfg_whiteout, cfg_fire,
        cfg_ground, cfg_flames, cfg_cinema, cfg_debug);

    register_shop_item();
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
