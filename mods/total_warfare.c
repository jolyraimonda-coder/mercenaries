/* TOTAL WARFARE OVERHAUL v2.9.0 - a code mod for Mercenaries Recompiled 1.1.1
 * (Mod Manager / Mod API v2 + Native Effects / MercFx).
 *
 * World in Flames cutscene nukes (the opening movie look):
 *  - fire-dominant mushroom cap: flat-topped orange fire dome, warm red-brown
 *    smoke, wide dark skirt, thick fiery stem, bobbing, over an orange sky;
 *  - low orange terrain-hugging fire wave sweeping 10 s to ~1430 units;
 *  - wave demolition: retail explosions crumble buildings at the wavefront;
 *  - Meltdown primary blast (r=1100) and 1K city blast radius;
 *  - white-out flash (8 stacked screen-filling passes) + white-hot core;
 *  - layered nuke audio, earthquake shake, slow motion, enhanced airstrikes,
 *    catch-all weapon enhancement, scorch, shop item, debug logging.
 * Without Native Effects every bomb keeps its vanilla blast.
 *
 * Build: clang --target=x86_64-pc-windows-msvc -O2 -LD (link kernel32+ucrt)
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

#define MOD_NAME            "TOTAL WARFARE OVERHAUL"
#define MOD_VERSION         "2.9.0"
#define MOD_INI             "mods\\total_warfare.ini"
#define MOD_INI_SECTION     "TotalWarfare"

#define NUKE_EFFECT_NAME    "totalwarfare.main"
#define NUKE_OWNER_KEY      "totalwarfare"
#define SHOP_TEMPLATE       "template_mafia_support_frog"  /* delivers template_amm_meltdown */
#define SHOP_TEXTURE        "shop_bunkerbuster"
#define SHOP_NAME_KEY       "support.airstrikes.totalwarfare"
#define SHOP_SHORT_KEY      "shop.totalwarfare.short"
#define SHOP_LONG_KEY       "shop.totalwarfare.long"
#define SHOP_PRICE          90000u

/* Retail addresses (Mod Manager documentation). */
#define SHOP_OBJECT          0x0034ABC8u
#define SHOP_STOCKED_FN      0x000C8BF0u
#define SHOP_STOCK_FN        0x000CDA60u
#define SPRITE_BEGIN_FN      0x0020A0D0u
#define SPRITE_QUAD_FN       0x0020C980u
#define EXPLOSION_FN         0x000B6460u   /* RsExplosion::Explode */

/* Cached game texture hashes for the custom particle system. */
#define TEX_FLASH_HASH       0x88e27c45u
#define TEX_EXPLOSION_HASH   0x02bb1fe0u
#define TEX_DUST_HASH        0xc272d7afu

#define BLAST_SLOTS          3u
#define EXTRA_QUEUE          32u   /* demolition guest_calls budget per wave */

#define FLASH_MAX_BRUSHES    96u
#define FLASH_PASSES         8u    /* stacked screen-filling flashes */

/* Weapon template categories. */
#define CAT_SMART   0u
#define CAT_CARPET  1u
#define CAT_CRUISE  2u   /* fuel-air bombs: FOAB treatment */
#define CAT_BB      3u
#define CAT_C4      4u
#define CAT_WEAPON  5u   /* catch-all: rocket pods, tank busters, unknown */

typedef struct { const char *name; unsigned char cat; } TriggerTemplate;

static const TriggerTemplate trigger_templates[] = {
    /* Bunker busters */
    {"template_amm_bunkerbuster",      CAT_BB},
    {"template_amm_bunkerbuster0",     CAT_BB},
    {"template_amm_bunkerbusterbomb",  CAT_BB},
    /* Fuel-air bombs (FOAB treatment, cruise-category visuals) */
    {"template_amm_fuelairbomb",       CAT_CRUISE},
    {"template_amm_fuelairbomb0",      CAT_CRUISE},
    /* Cruise missiles */
    {"template_amm_cruisemissile",     CAT_CRUISE},
    {"template_amm_cruisemissile0",    CAT_CRUISE},
    /* Smart / precision */
    {"template_amm_smartbomb",         CAT_SMART},
    {"template_amm_smartbomb0",        CAT_SMART},
    {"template_amm_smartbomb2",        CAT_SMART},
    {"template_amm_smartbomb3",        CAT_SMART},
    {"template_amm_hvysmartbomb",      CAT_SMART},
    {"template_amm_hvysmartbomb0",     CAT_SMART},
    {"template_amm_guidedmissile",     CAT_SMART},
    {"template_amm_guidedmissile0",    CAT_SMART},
    {"template_amm_surgicalstrike",    CAT_SMART},
    {"template_amm_surgicalstrike0",   CAT_SMART},
    /* Carpet / cluster / artillery */
    {"template_amm_carpetbomb",        CAT_CARPET},
    {"template_amm_carpetbomb0",       CAT_CARPET},
    {"template_amm_carpetbomblet",     CAT_CARPET},
    {"template_amm_clusterbomb",       CAT_CARPET},
    {"template_amm_clusterbomb0",      CAT_CARPET},
    {"template_amm_clusterbombBomb",   CAT_CARPET},
    {"template_amm_clusterbomblet",    CAT_CARPET},
    {"template_amm_clusterbomblet0",   CAT_CARPET},
    {"template_amm_artillery",         CAT_CARPET},
    {"template_amm_artillery0",        CAT_CARPET},
    {"template_amm_artillery2",        CAT_CARPET},
    {"template_amm_artillery3",        CAT_CARPET},
    {"template_amm_airsuperiority",    CAT_CARPET},
    {"template_amm_airsuperiority0",   CAT_CARPET},
    /* Weapon-grade ordnance (catch-all group) */
    {"template_amm_tankbuster",        CAT_WEAPON},
    {"template_amm_tankbuster0",       CAT_WEAPON},
    {"template_amm_rocket",            CAT_WEAPON},
    {"template_amm_rockets",           CAT_WEAPON},
    {"template_amm_helirocket",        CAT_WEAPON},
    {"template_amm_helirockets",       CAT_WEAPON},
    {"template_amm_rpg",               CAT_WEAPON},
    {"template_amm_rpg7",              CAT_WEAPON},
    {"template_amm_grenadelauncher",   CAT_WEAPON},
    {"template_amm_mortar",            CAT_WEAPON},
    {"template_amm_mortar0",           CAT_WEAPON},
    /* C4 */
    {"template_amm_c4",                CAT_C4},
    {"template_amm_c40",               CAT_C4},
    {"template_amm_remotec4",          CAT_C4},
    {"template_amm_remotec40",         CAT_C4},
    {"template_amm_satchel",           CAT_C4},
};
#define TRIGGER_TEMPLATE_COUNT (sizeof(trigger_templates)/sizeof(trigger_templates[0]))
#define TEMPLATE_SLOTS 64u

/* Custom particle types (registered from the game's cached textures). */
enum { TSAR_FLASH, TSAR_FIRE, TSAR_SMOKE, TSAR_DUST, TSAR_PLUME, TSAR_GROUND,
       TSAR_WAVE, TSAR_GLOW, TSAR_PARTICLES };

typedef struct {
    int active;
    int wide;              /* MEGA cap */
    float pos[3];
    float age;
    float duration;
    float next_ring;       /* shockwave rings */
    float next_scorch;
    float next_cap;        /* mushroom cap batch timer */
    float next_stem;       /* stem batch timer */
    float next_wave;       /* fire wave batch timer */
    float next_demo;       /* wave demolition strike timer */
    unsigned scorch_step;
    unsigned demo_angle;   /* advancing demolition angle index */
    unsigned strikes;      /* demolition strikes fired */
    int tsar;              /* TSAR tier flag */
    float tsar_sample;     /* cached terrain height */
    int tsar_pending;
    uint32_t rise, shock;  /* cue handles */
} BlastSlot;

static BlastSlot slots[BLAST_SLOTS];
static unsigned slot_cursor;

/* ------------------------------------------------------------------- state */

static const MercenariesModHost *host;
static const MercFxApi *fx;

static unsigned cfg_master = 1;
static unsigned cfg_smart  = 1;   /* SmartBomb */
static unsigned cfg_surgical = 1; /* SurgicalStrike */
static unsigned cfg_carpet = 1;   /* CarpetBomb */
static unsigned cfg_bb     = 2;   /* BunkerBuster: 0 off, 1 enhanced, 2 NUKE */
static unsigned cfg_foab   = 3;   /* CruiseFOAB: 0 off, 1 enhanced, 2 FOAB, 3 MEGA */
static unsigned cfg_strategic = 3;/* StrategicNuke: 0 off, 1 on, 2 TSAR, 3 MEGA */
static unsigned cfg_c4     = 3;   /* C4Mega: 0 off, 1 lite, 2 mega, 3 extreme */
static unsigned cfg_realism = 1;  /* WeaponRealism */
static unsigned cfg_quake  = 2;   /* Earthquake: 0 off, 1 strong 10s, 2 extreme 20s */
static unsigned cfg_audio  = 2;   /* NukeAudio: 0 off, 1 layered, 2 nuke only */
static unsigned cfg_demo   = 1;   /* WaveDemo */
static unsigned cfg_effect = 1;   /* NukeEffect: 0 classic, 1 meltdown */
static unsigned cfg_shop   = 2;   /* ShopProfile: 1 off, 2 on */
static unsigned cfg_debug  = 0;   /* DebugLog */
static float view_range = 4096.0f;/* ini-only ViewRange 128..4096 */

static int attach_state;          /* 0 untried, 1 pending, 2 attached, 3 given up */
static unsigned attach_attempts;
static unsigned module_misses;
static int logged_missile_advisory;

static uint32_t sound_primary;    /* a_strike.bb_explosion */
static uint32_t sound_fae;        /* a_strike.fae_init_exp */
static uint32_t sound_shock;      /* a_strike.fae_shockwave */
static uint32_t sound_launch;     /* base.nuke_launch_exp */

static uint32_t template_hash[TEMPLATE_SLOTS];
static unsigned char bound_flags[TEMPLATE_SLOTS];
static unsigned bound_count;

static uint32_t particles[TSAR_PARTICLES];
static int particles_ready;
static unsigned prepared_count;

static uint32_t flash_texture;
static float flash_time = -1.0f;
static float flash_hold;
static float flash_fade;
static unsigned brushes_this_frame;
static unsigned brushes_last_frame;

static int retail_addresses_checked;
static int have_begin, have_quad, have_stocked, have_stock, have_explosion_fn;
static uint32_t meltdown_visual;
static double world_time;
static double next_rebind_time;
static double next_stock_time;
static unsigned stock_attempts;
static int pending_log_blast_failure;
static char pending_log_trigger[64];
static int pending_explosion;
static float pending_explosion_pos[3];
static float pending_explosion_radius;
static int pending_strategic_hit;  /* engine-scripted advisory */

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

#define LOG(...) do { if (host && host->log) host->log("total_warfare", __VA_ARGS__); } while (0)
#define DLOG(...) do { if (cfg_debug && host && host->log) host->log("tw_debug", __VA_ARGS__); } while (0)

static float rand_frac(unsigned *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return (float)(*seed & 0x00FFFFFFu) / 16777216.0f;
}

static unsigned cat_of(unsigned index)
{
    if (index >= (unsigned)TRIGGER_TEMPLATE_COUNT) return CAT_WEAPON;
    return trigger_templates[index].cat;
}

static unsigned cat_allows(unsigned cat)
{
    switch (cat) {
    case CAT_SMART:   return cfg_smart;
    case CAT_CARPET:  return cfg_carpet;
    case CAT_CRUISE:  return cfg_foab != 0;
    case CAT_BB:      return cfg_bb != 0;
    case CAT_C4:      return cfg_c4 != 0;
    default:          return cfg_realism;  /* CAT_WEAPON: catch-all realism */
    }
}

static void bind_pass(int announce)
{
    unsigned i, ok_count = 0;
    if (!fx || !fx->bind_template) return;
    if ((unsigned)TRIGGER_TEMPLATE_COUNT > TEMPLATE_SLOTS) return;
    for (i = 0; i < (unsigned)TRIGGER_TEMPLATE_COUNT; ++i) {
        unsigned char ok = fx->bind_template(trigger_templates[i].name, NUKE_EFFECT_NAME,
                                             (int)cat_allows(trigger_templates[i].cat)) ? 1u : 0u;
        template_hash[i] = fnv1a_casefold(trigger_templates[i].name);
        if (ok) ++ok_count;
        if (ok != bound_flags[i] || announce)
            LOG("bind %u %s: %s", i, trigger_templates[i].name,
                ok ? "BOUND" : "not bound (another mod owns it, or absent)");
        bound_flags[i] = ok;
    }
    bound_count = ok_count;
    if (announce) {
        LOG("bind summary: %u of %u trigger templates bound",
            bound_count, (unsigned)TRIGGER_TEMPLATE_COUNT);
        if (!bound_flags[0]) {
            /* The strategic missile's detonation is engine-scripted and is
               never intercepted; say so once. */
            if (!logged_missile_advisory) {
                logged_missile_advisory = 1;
                LOG("strategic missile detonation is engine-scripted (never intercepted); "
                    "use Bunker Buster NUKE or Fuel-Air FOAB for the nuke");
            }
        }
    }
}

/* -------------------------------------------------------- particle setup */

static void register_type(unsigned index, const MercFxParticle *spec)
{
    uint32_t handle;
    if (!fx || !fx->register_particle || !fx->prepare_particle) return;
    handle = fx->register_particle(spec);
    if (handle && fx->prepare_particle(handle)) {
        particles[index] = handle;
        DLOG("particle %u prepared (tex %08X)", index, spec->texture_hash);
    } else {
        particles[index] = 0;
    }
}

static void try_register_particles(void)
{
    MercFxParticle s;
    if (!fx || particles_ready) return;
    if (!fx->register_particle || !fx->prepare_particle) return;

    /* FLASH: blinding white-yellow burst (whiteout + white-hot core). */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_FLASH_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0xC0FFFFFFu; s.colors[1] = 0x90FFF2D0u; s.colors[2] = 0x00FFEFC0u;
    s.widths[0] = 40.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 130.0f; s.heights[1] = 130.0f;
    s.widths[2] = 260.0f; s.heights[2] = 260.0f;
    s.time_b = 0.2f;
    s.y_acceleration = 2.0f; s.friction = 0.5f;
    s.lifetime = 0.6f; s.lifetime_random = 0.2f;
    s.max_distance = view_range;
    register_type(TSAR_FLASH, &s);

    /* FIRE: bright yellow-white fading to orange (cutscene early cap). */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_EXPLOSION_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0xB0FFFFD8u; s.colors[1] = 0x90FFC840u; s.colors[2] = 0x30E06000u;
    s.widths[0] = 40.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 95.0f;  s.heights[1] = 95.0f;
    s.widths[2] = 115.0f; s.heights[2] = 115.0f;
    s.time_b = 0.25f;
    s.y_acceleration = 6.0f; s.friction = 0.3f;
    s.lifetime = 5.5f; s.lifetime_random = 1.5f;
    s.random_scale = 0.4f; s.random_acceleration = 3.0f;
    s.max_distance = view_range;
    register_type(TSAR_FIRE, &s);

    /* GLOW: giant soft yellow ambient light around the cap (cutscene orange sky). */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_FLASH_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x30FFEE90u; s.colors[1] = 0x18FFD840u; s.colors[2] = 0x00C08020u;
    s.widths[0] = 150.0f; s.heights[0] = 150.0f;
    s.widths[1] = 420.0f; s.heights[1] = 420.0f;
    s.widths[2] = 620.0f; s.heights[2] = 620.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 1.0f; s.friction = 0.2f;
    s.lifetime = 6.0f; s.lifetime_random = 1.0f;
    s.max_distance = view_range;
    register_type(TSAR_GLOW, &s);

    /* SMOKE: warm red-brown, near-black at the end (photo look). */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_DUST_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x804A2C20u; s.colors[1] = 0x80341E16u; s.colors[2] = 0x0018100Cu;
    s.widths[0] = 50.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 140.0f; s.heights[1] = 110.0f;
    s.widths[2] = 220.0f; s.heights[2] = 170.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 4.0f; s.friction = 0.2f;
    s.lifetime = 9.0f; s.lifetime_random = 2.0f;
    s.random_scale = 0.35f; s.random_acceleration = 2.0f;
    s.wind = 1.5f;
    s.max_distance = view_range;
    register_type(TSAR_SMOKE, &s);

    /* DUST: tan-grey ground dust ring. */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_DUST_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x70BBAA88u; s.colors[1] = 0x50998877u; s.colors[2] = 0x20776655u;
    s.widths[0] = 40.0f;  s.heights[0] = 15.0f;
    s.widths[1] = 80.0f;  s.heights[1] = 35.0f;
    s.widths[2] = 100.0f; s.heights[2] = 55.0f;
    s.time_b = 0.4f;
    s.y_acceleration = 1.5f; s.friction = 0.4f;
    s.lifetime = 8.0f; s.lifetime_random = 2.0f;
    s.max_distance = view_range;
    register_type(TSAR_DUST, &s);

    /* PLUME: dark red-brown stem smoke, slow rise. */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_DUST_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x8042261Cu; s.colors[1] = 0x602E1A12u; s.colors[2] = 0x0016100Cu;
    s.widths[0] = 25.0f;  s.heights[0] = 40.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 85.0f;
    s.widths[2] = 95.0f;  s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 5.0f; s.friction = 0.25f;
    s.lifetime = 10.0f; s.lifetime_random = 2.0f;
    s.max_distance = view_range;
    register_type(TSAR_PLUME, &s);

    /* GROUND: dark dust blanket drifting over the streets. */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_DUST_HASH;
    s.blend_mode = 0;
    s.colors[0] = 0x60554030u; s.colors[1] = 0x48382a20u; s.colors[2] = 0x10201a14u;
    s.widths[0] = 90.0f;  s.heights[0] = 25.0f;
    s.widths[1] = 320.0f; s.heights[1] = 70.0f;
    s.widths[2] = 800.0f; s.heights[2] = 140.0f;
    s.time_b = 0.35f;
    s.y_acceleration = 1.0f; s.friction = 0.3f;
    s.lifetime = 14.0f; s.lifetime_random = 4.0f;
    s.wind = 3.0f;
    s.max_distance = view_range;
    register_type(TSAR_GROUND, &s);

    /* WAVE: bright orange fire wall hugging the terrain. */
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.texture_hash = TEX_EXPLOSION_HASH;
    s.blend_mode = 1;
    s.colors[0] = 0x80FFB030u; s.colors[1] = 0x70FF7010u; s.colors[2] = 0x30A02800u;
    s.widths[0] = 25.0f;  s.heights[0] = 25.0f;
    s.widths[1] = 60.0f;  s.heights[1] = 60.0f;
    s.widths[2] = 100.0f; s.heights[2] = 100.0f;
    s.time_b = 0.3f;
    s.y_acceleration = 3.0f; s.friction = 0.35f;
    s.lifetime = 3.5f; s.lifetime_random = 1.0f;
    s.random_scale = 0.4f;
    s.max_distance = view_range;
    register_type(TSAR_WAVE, &s);

    particles_ready = 1;
    prepared_count = 0;
    for (s.size = 0; s.size < TSAR_PARTICLES; ++s.size) {
        if (particles[(unsigned)s.size]) ++prepared_count;
        else particles_ready = 0;
    }
    LOG("tsar particle system armed: %u of 8 types prepared", prepared_count);
    if (!particles_ready)
        LOG("some particle textures not cached; the cloud still builds from what is ready");
}

/* ------------------------------------------------------- MercFx timeline */

static void emit(unsigned type, const float p[3], const float v[3])
{
    if (particles[type] && fx->emit_particle)
        fx->emit_particle(particles[type], p, v, 0.15f, 1.0f);
}

/* Mushroom cap: 24 sprites per batch, fire-dominant (Merc-2 photo). */
static void emit_cap(BlastSlot *slot, float dt)
{
    float p[3], v[3];
    const float cap_r = slot->wide ? 85.0f : 70.0f;
    const float cap_h = slot->wide ? 110.0f : 90.0f;
    const float deck = cap_h + sinf(slot->age * 1.05f) * 7.0f;   /* bobbing +-7 */
    const float unfurl = slot->age < 15.0f ? slot->age / 15.0f : 1.0f;
    unsigned seed = (unsigned)(slot->age * 917.0f) + 7u;
    unsigned j;

    slot->next_cap -= dt;
    if (slot->next_cap > 0.0f) return;
    slot->next_cap = 0.09f;

    for (j = 0; j < 24; ++j) {
        float angle = 6.2832f * rand_frac(&seed);
        float radius = cap_r * unfurl * sqrtf(rand_frac(&seed));
        p[0] = slot->pos[0] + cosf(angle) * radius;
        p[2] = slot->pos[2] + sinf(angle) * radius;
        if (j < 14) {
            /* FIRE-DOMINANT head: glowing dome, dark under-belly (cutscene) */
            p[1] = slot->pos[1] + deck - 12.0f + 26.0f * rand_frac(&seed);
            v[1] = (slot->age < 6.0f) ? 9.0f : 1.5f;
            v[0] = cosf(angle) * 3.0f;
            v[2] = sinf(angle) * 3.0f;
            emit(TSAR_FIRE, p, v);
        } else if (j < 20) {
            /* red-brown smoke in the gaps between flames */
            radius = cap_r * unfurl * sqrtf(rand_frac(&seed));
            p[0] = slot->pos[0] + cosf(angle) * radius;
            p[2] = slot->pos[2] + sinf(angle) * radius;
            p[1] = slot->pos[1] + deck - 10.0f + 6.0f * rand_frac(&seed);
            v[1] = (slot->age < 6.0f) ? 9.0f : 1.0f;
            v[0] = cosf(angle) * 3.0f;
            v[2] = sinf(angle) * 3.0f;
            emit(TSAR_SMOKE, p, v);
        } else if (j < 22) {
            /* wide dark skirt under the head */
            radius = cap_r * 1.35f * unfurl * sqrtf(rand_frac(&seed));
            p[0] = slot->pos[0] + cosf(angle) * radius;
            p[2] = slot->pos[2] + sinf(angle) * radius;
            p[1] = slot->pos[1] + deck - 16.0f + 4.0f * rand_frac(&seed);
            v[1] = (slot->age < 6.0f) ? 9.0f : 0.8f;
            v[0] = cosf(angle) * 2.0f;
            v[2] = sinf(angle) * 2.0f;
            emit(TSAR_SMOKE, p, v);
        } else {
            /* deep dark underside edge */
            radius = cap_r * 1.1f * unfurl * sqrtf(rand_frac(&seed));
            p[0] = slot->pos[0] + cosf(angle) * radius;
            p[2] = slot->pos[2] + sinf(angle) * radius;
            p[1] = slot->pos[1] + deck - 20.0f + 4.0f * rand_frac(&seed);
            v[1] = (slot->age < 6.0f) ? 9.0f : 0.6f;
            v[0] = cosf(angle) * 1.5f;
            v[2] = sinf(angle) * 1.5f;
            emit(TSAR_SMOKE, p, v);
        }
    }
}

/* Thick fiery stem: 3 dark plumes + 3 fire patches burning inside. */
static void emit_stem(BlastSlot *slot, float dt)
{
    float p[3], v[3];
    const float cap_h = slot->wide ? 110.0f : 90.0f;
    unsigned seed = (unsigned)(slot->age * 571.0f) + 31u;
    unsigned j;

    slot->next_stem -= dt;
    if (slot->next_stem > 0.0f) return;
    slot->next_stem = 0.11f;

    for (j = 0; j < 3; ++j) {
        float angle = 6.2832f * rand_frac(&seed);
        float radius = 10.0f + 12.0f * rand_frac(&seed);
        p[0] = slot->pos[0] + cosf(angle) * radius;
        p[1] = slot->pos[1] + 3.0f + (cap_h * 0.9f) * rand_frac(&seed);
        p[2] = slot->pos[2] + sinf(angle) * radius;
        v[0] = cosf(angle) * 0.5f;
        v[1] = 6.0f + 4.0f * rand_frac(&seed);
        v[2] = sinf(angle) * 0.5f;
        emit(TSAR_PLUME, p, v);
    }
    for (j = 0; j < 4; ++j) {
        float angle = 6.2832f * rand_frac(&seed);
        float radius = 8.0f + 10.0f * rand_frac(&seed);
        /* fire concentrated where the stem meets the cap (cutscene look) */
        float top_bias = (j < 2) ? 0.85f : 0.45f;
        p[0] = slot->pos[0] + cosf(angle) * radius;
        p[1] = slot->pos[1] + 3.0f + (cap_h * top_bias) * rand_frac(&seed);
        p[2] = slot->pos[2] + sinf(angle) * radius;
        v[0] = 0.0f; v[1] = 5.0f; v[2] = 0.0f;
        emit(TSAR_FIRE, p, v);
    }
}

/* Low orange fire wave: 24 sprites/batch hugging the terrain, grey layer above. */
static void emit_wave(BlastSlot *slot, float dt)
{
    float p[3], v[3], terrain;
    const float radius = 30.0f + slot->age * 140.0f;
    unsigned seed = (unsigned)(slot->age * 733.0f) + 97u;
    unsigned j;

    slot->next_wave -= dt;
    if (slot->next_wave > 0.0f) return;
    slot->next_wave = 0.08f;

    for (j = 0; j < 24; ++j) {
        float angle = (6.2832f * (float)j) / 24.0f + rand_frac(&seed) * 0.12f;
        float jitter = radius + (rand_frac(&seed) - 0.5f) * 30.0f;
        float x = slot->pos[0] + cosf(angle) * jitter;
        float z = slot->pos[2] + sinf(angle) * jitter;
        terrain = slot->pos[1];
        if (fx->sample_terrain_height) {
            float h;
            if (fx->sample_terrain_height(x, z, &h)) terrain = h;
        }
        p[0] = x; p[2] = z;
        p[1] = terrain + 2.5f;                    /* LOW: hugs the ground */
        v[0] = cosf(angle) * 30.0f;
        v[1] = 4.0f;
        v[2] = sinf(angle) * 30.0f;
        emit(TSAR_WAVE, p, v);
        p[1] = terrain + 10.0f;                   /* grey layer above */
        v[1] = 2.0f;
        emit(TSAR_SMOKE, p, v);
    }
}

static void __cdecl tw_timeline_tick(float delta_seconds, void *userdata)
{
    unsigned i;
    (void)userdata;
    if (!cfg_master) return;
    for (i = 0; i < BLAST_SLOTS; ++i) {
        BlastSlot *slot = &slots[i];
        if (!slot->active) continue;
        slot->age += delta_seconds;

        if (slot->rise && slot->age >= 0.30f) {
            if (fx->sound_ready && fx->sound_ready(slot->rise))
                fx->play_sound(slot->rise, slot->pos);
            slot->rise = 0;
        }
        if (slot->shock && slot->age >= 0.9f) {
            if (fx->sound_ready && fx->sound_ready(slot->shock))
                fx->play_sound(slot->shock, slot->pos);
            slot->shock = 0;
        }

        /* white-hot core: blinding glow at the center for the first 4 s */
        if (slot->age < 4.0f) {
            float p[3], v[3];
            unsigned seed = (unsigned)(slot->age * 431.0f) + 3u;
            unsigned j;
            for (j = 0; j < 2; ++j) {
                p[0] = slot->pos[0];
                p[2] = slot->pos[2];
                p[1] = slot->pos[1] + 6.0f + 14.0f * rand_frac(&seed);
                v[0] = v[1] = v[2] = 0.0f;
                emit(TSAR_FLASH, p, v);
            }
        }

        /* ambient sky glow: giant soft yellow light for the first seconds */
        if (slot->age < 8.0f && particles[TSAR_GLOW]) {
            float p[3], v[3] = {0.0f, 2.0f, 0.0f};
            unsigned gseed = (unsigned)(slot->age * 977.0f) + 5u;
            const float cap_h = slot->wide ? 110.0f : 90.0f;
            p[0] = slot->pos[0] + (rand_frac(&gseed) - 0.5f) * 60.0f;
            p[1] = slot->pos[1] + cap_h + 10.0f;
            p[2] = slot->pos[2] + (rand_frac(&gseed) - 0.5f) * 60.0f;
            emit(TSAR_GLOW, p, v);
        }

        emit_cap(slot, delta_seconds);
        emit_stem(slot, delta_seconds);
        emit_wave(slot, delta_seconds);

        /* ground dust blanket */
        {
            float p[3], v[3];
            unsigned seed = (unsigned)(slot->age * 311.0f) + 13u;
            unsigned j;
            slot->next_scorch -= delta_seconds;   /* reuse as dust timer */
            if (slot->next_scorch <= 0.0f) {
                slot->next_scorch = 0.30f;
                for (j = 0; j < 2; ++j) {
                    float angle = 6.2832f * rand_frac(&seed);
                    float radius = (40.0f + slot->age * 90.0f) * sqrtf(rand_frac(&seed));
                    p[0] = slot->pos[0] + cosf(angle) * radius;
                    p[1] = slot->pos[1] + 4.0f;
                    p[2] = slot->pos[2] + sinf(angle) * radius;
                    v[0] = cosf(angle) * 8.0f; v[1] = 3.0f; v[2] = sinf(angle) * 8.0f;
                    emit(TSAR_GROUND, p, v);
                }
            }
        }

        /* rolling shockwave rings while the wave sweeps */
        slot->next_ring -= delta_seconds;
        if (slot->next_ring <= 0.0f && fx->emit_shockwave) {
            MercFxShockwave shock;
            slot->next_ring = 2.0f;
            if (slot->age < slot->duration * 0.6f) {
                shock.size = sizeof(shock);
                shock.flags = slot->age > 0.5f ? MERCFX_SHOCKWAVE_RINGS
                                               : (MERCFX_SHOCKWAVE_REFRACTION | MERCFX_SHOCKWAVE_RINGS);
                shock.radius = 200.0f;
                shock.duration = 5.0f;
                fx->emit_shockwave(slot->pos, &shock);
            }
        }

        if (slot->age >= slot->duration) slot->active = 0;
    }
}

static void __cdecl tw_timeline_reset(void *userdata)
{
    (void)userdata;
    memset(slots, 0, sizeof(slots));
    flash_time = -1.0f;
}

/* ------------------------------------------------------------- attach */

static MercFxDecision __cdecl tw_impact(const MercFxImpact *event, void *userdata);

static int try_attach(void)
{
    HMODULE module;
    MercFxGetApi get_api;

    if (!host || attach_state == 2 || attach_state == 3) return attach_state == 2;
    module = GetModuleHandleW(L"native_effects.dll");
    if (!module) {
        if (++module_misses >= 600) {
            attach_state = 3;
            LOG("native_effects.dll not loaded; Total Warfare stays vanilla");
        }
        return 0;
    }
    get_api = (MercFxGetApi)(void *)GetProcAddress(module, "MercFx_GetApi");
    if (!get_api) {
        attach_state = 3;
        LOG("native_effects.dll has no MercFx_GetApi; staying vanilla");
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
        !fx->add_ground_scorch) {
        attach_state = 3;
        LOG("MercFx API table incomplete (%u bytes); staying vanilla", fx->size);
        return 0;
    }

    if (!fx->register_effect(NUKE_EFFECT_NAME, tw_impact, NULL)) {
        if (++attach_attempts >= 3) {
            attach_state = 3;
            LOG("MercFx effect registration failed; staying vanilla");
        }
        return 0;
    }
    attach_state = 2;
    LOG("MercFx attached; binding triggers and cues");

    bind_pass(1);

    /* MOAB ships its own native_effects.dll; if it replaced the standard one,
       say so plainly so a broken setup is obvious in the log. */
    {
        char path[MAX_PATH];
        const char *slash = NULL;
        DWORD n = GetModuleFileNameA(module, path, MAX_PATH), i;
        if (n > 0 && n < MAX_PATH) {
            for (i = n; i > 0; --i) if (path[i-1] == '\\') { slash = &path[i-1] + 1; break; }
            if (slash) LOG("native_effects.dll in use: %s", slash);
        }
    }

    sound_primary = fx->register_sound("a_strike.bb_explosion");
    sound_fae     = fx->register_sound("a_strike.fae_init_exp");
    sound_shock   = fx->register_sound("a_strike.fae_shockwave");
    sound_launch  = fx->register_sound("base.nuke_launch_exp");
    LOG("cues: bb=%u fae=%u shock=%u launch=%u (0 = unavailable)",
        sound_primary, sound_fae, sound_shock, sound_launch);

    if (!fx->register_simulation(tw_timeline_tick, tw_timeline_reset, NULL))
        LOG("timeline registration failed; custom cloud off");

    LOG("armed: bound=%u of %u, master=%u, strategic=%u, quake=%u, audio=%u, demo=%u",
        bound_count, (unsigned)TRIGGER_TEMPLATE_COUNT, cfg_master,
        cfg_strategic, cfg_quake, cfg_audio, cfg_demo);
    LOG("nuke cloud view range: %.0f", view_range);

    try_register_particles();
    if (!bound_count)
        LOG("no trigger template bound; nukes will not fire until one binds (retrying every 2 s)");
    return 1;
}

/* ------------------------------------------------------------ settings rows */

static unsigned get_master(void)   { return cfg_master; }
static unsigned get_smart(void)    { return cfg_smart; }
static unsigned get_surgical(void) { return cfg_surgical; }
static unsigned get_carpet(void)   { return cfg_carpet; }
static unsigned get_bb(void)       { return cfg_bb; }
static unsigned get_foab(void)     { return cfg_foab; }
static unsigned get_strategic(void){ return cfg_strategic; }
static unsigned get_c4(void)       { return cfg_c4; }
static unsigned get_realism(void)  { return cfg_realism; }
static unsigned get_quake(void)    { return cfg_quake; }
static unsigned get_audio(void)    { return cfg_audio; }
static unsigned get_demo(void)     { return cfg_demo; }
static unsigned get_effect(void)   { return cfg_effect; }
static unsigned get_shop(void)     { return cfg_shop; }
static unsigned get_debug(void)    { return cfg_debug; }

static void write_unsigned(const char *key, unsigned value)
{
    if (host && host->write_setting) host->write_setting(MOD_INI, MOD_INI_SECTION, key, value);
}

static void apply_bindings(void)
{
    unsigned i;
    if (!fx || attach_state != 2 || !fx->set_template_enabled) return;
    for (i = 0; i < (unsigned)TRIGGER_TEMPLATE_COUNT; ++i)
        if (!fx->set_template_enabled(trigger_templates[i].name,
                                      (int)cat_allows(trigger_templates[i].cat)))
            DLOG("cannot switch %s (not bound in this game)", trigger_templates[i].name);
}

#define SET_on(name, var) \
    static void set_##var(unsigned v) { cfg_##var = v & 1u; write_unsigned(name, cfg_##var); apply_bindings(); }

SET_on("Master", master)
SET_on("SmartBomb", smart)
SET_on("SurgicalStrike", surgical)
SET_on("CarpetBomb", carpet)
SET_on("WeaponRealism", realism)
SET_on("WaveDemo", demo)

static void set_bb(unsigned v)
{
    if (v > 2u) v = 2u;
    cfg_bb = v; write_unsigned("BunkerBuster", v); apply_bindings();
}
static void set_foab(unsigned v)
{
    if (v > 3u) v = 3u;
    cfg_foab = v; write_unsigned("CruiseFOAB", v); apply_bindings();
}
static void set_strategic(unsigned v)
{
    if (v > 3u) v = 3u;
    cfg_strategic = v; write_unsigned("StrategicNuke", v);
}
static void set_c4(unsigned v)
{
    if (v > 3u) v = 3u;
    cfg_c4 = v; write_unsigned("C4Mega", v); apply_bindings();
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
static void set_effect(unsigned v)
{
    if (v > 1u) v = 1u;
    cfg_effect = v; write_unsigned("NukeEffect", v);
}
static void set_shop(unsigned v)
{
    if (v > 2u) v = 2u;
    cfg_shop = v; write_unsigned("ShopProfile", v);
}
static void set_debug(unsigned v)
{
    if (v > 1u) v = 1u;
    cfg_debug = v; write_unsigned("DebugLog", v);
}

static const char *const on_off[] = {"OFF", "ON"};
static const char *const bb_labels[] = {"OFF", "ENHANCED", "NUKE"};
static const char *const foab_labels[] = {"OFF", "ENHANCED", "FOAB", "MEGA"};
static const char *const strategic_labels[] = {"OFF", "ON", "TSAR", "MEGA"};
static const char *const c4_labels[] = {"OFF", "LITE", "MEGA", "EXTREME"};
static const char *const quake_labels[] = {"OFF", "STRONG", "EXTREME"};
static const char *const audio_labels[] = {"OFF", "LAYERED", "NUKE ONLY"};
static const char *const effect_labels[] = {"CLASSIC", "MELTDOWN"};
static const char *const shop_labels[] = {"OFF", "ON"};

#define SETTING_COUNT 15u

static const MercenariesModSetting settings[SETTING_COUNT] = {
    {"MASTER",            on_off, 2, get_master,    set_master},
    {"SMART BOMB",        on_off, 2, get_smart,     set_smart},
    {"SURGICAL STRIKE",   on_off, 2, get_surgical,  set_surgical},
    {"CARPET BOMB",       on_off, 2, get_carpet,    set_carpet},
    {"BUNKER BUSTER",     bb_labels, 3, get_bb,     set_bb},
    {"FUEL-AIR CRUISE",   foab_labels, 4, get_foab,  set_foab},
    {"STRATEGIC NUKE",    strategic_labels, 4, get_strategic, set_strategic},
    {"C4 MEGA",           c4_labels, 4, get_c4,       set_c4},
    {"WEAPON REALISM",    on_off, 2, get_realism,   set_realism},
    {"EARTHQUAKE",        quake_labels, 3, get_quake,  set_quake},
    {"NUKE AUDIO",        audio_labels, 3, get_audio,  set_audio},
    {"WAVE DEMOLITION",   on_off, 2, get_demo,      set_demo},
    {"NUKE EFFECT",       effect_labels, 2, get_effect, set_effect},
    {"SHOP PROFILE",      shop_labels, 2, get_shop,    set_shop},
    {"DEBUG LOG",         on_off, 2, get_debug,     set_debug},
};

static const uint8_t setting_controls[SETTING_COUNT] = {
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER,
};

static const unsigned setting_defaults[SETTING_COUNT] = {
    1u, 1u, 1u, 1u, 2u, 3u, 3u, 3u, 1u, 2u, 2u, 1u, 1u, 2u, 0u,
};

static const char *const setting_descriptions[SETTING_COUNT] = {
    "Turns the whole overhaul on and off. OFF keeps every bomb vanilla.",
    "Smart and guided bombs hit harder and wider.",
    "The Surgical Strike becomes a precision bunker-buster blast.",
    "Carpet, cluster and artillery strikes dig massive craters.",
    "ENHANCED scales the bunker buster. NUKE turns it into a full nuke strike.",
    "ENHANCED scales cruise missiles. FOAB/MEGA gives the fuel-air bomb city-crushing yield.",
    "The mushroom tier for every nuke: ON, TSAR (70x90 cap) or MEGA (85x110 cap).",
    "C4 charges from LITE to EXTREME city-shaking blasts.",
    "Every other explosive weapon hits about 60 percent harder and wider.",
    "EARTHQUAKE shake after every nuke: STRONG 10 s or EXTREME 20 s.",
    "NUKE ONLY plays the layered strike cues over the vanilla blast; LAYERED keeps both.",
    "Retail demolition explosions crumble buildings along the fire wave.",
    "MELTDOWN fires the retail Meltdown blast (r=1100) as the primary nuke blast.",
    "Adds the TOTAL WARFARE strike to the Merchant of Menace.",
    "Verbose log lines for tuning.",
};

/* --------------------------------------------------------------- shop setup */

static const MercenariesShopItem tw_item = {
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
        host->register_string(SHOP_NAME_KEY, "TOTAL WARFARE STRIKE");
        host->register_string(SHOP_SHORT_KEY, "Total warfare strike.");
        host->register_string(SHOP_LONG_KEY,
            "One bomber, one warhead pushed far past every limit. "
            "Call it in, take cover, and leave nothing standing.");
    }
    if (host->add_shop_item(&tw_item))
        LOG("shop item TOTAL WARFARE STRIKE added on %s", SHOP_TEMPLATE);
    else
        LOG("shop item refused (already sold, or the Mod Manager is too old)");
}

static void try_stock_item(void)
{
    uint32_t hash = fnv1a_casefold(SHOP_TEMPLATE);
    const uint32_t check_args[1] = {hash};
    const uint32_t stock_args[2] = {hash, 0u};

    if (!retail_addresses_checked) {
        have_stocked = host->guest_function_exists(SHOP_STOCKED_FN) ? 1 : 0;
        have_stock = host->guest_function_exists(SHOP_STOCK_FN) ? 1 : 0;
        have_begin = host->guest_function_exists(SPRITE_BEGIN_FN) ? 1 : 0;
        have_quad = host->guest_function_exists(SPRITE_QUAD_FN) ? 1 : 0;
        have_explosion_fn = host->guest_function_exists(EXPLOSION_FN) ? 1 : 0;
        meltdown_visual = fnv1a_casefold("Meltdown");
        retail_addresses_checked = 1;
    }
    if (!have_stock || cfg_shop != 2u) return;
    if (have_stocked && host->guest_call(SHOP_STOCKED_FN, 0u, 1u, check_args)) return;
    host->guest_call(SHOP_STOCK_FN, SHOP_OBJECT, 2u, stock_args);
    LOG("stocked TOTAL WARFARE STRIKE in the Merchant of Menace");
    next_stock_time = world_time + 30.0;
}

/* ----------------------------------------------------------- impact handler */

/* Earthquake: STRONG 10 s, EXTREME 20 s magnitude 5 (engine seconds). */
static void quake_opts(MercFxBlastOptions *options)
{
    if (cfg_quake == 1) { options->shake_duration = 10.0f; options->shake_magnitude = 3.0f; }
    else if (cfg_quake == 2) { options->shake_duration = 20.0f; options->shake_magnitude = 5.0f; }
}

/* Nuke audio: layered cues, and the default sound stays audible on LAYERED. */
static void audio_opts(MercFxBlastOptions *options, uint32_t *first_cue)
{
    *first_cue = 0;
    if (cfg_audio == 0) return;
    if (cfg_audio == 2 && sound_primary && fx->sound_ready(sound_primary)) {
        options->suppress_default_sound = 1u;      /* NUKE ONLY */
        *first_cue = sound_primary;
    }
    /* LAYERED: default blast sound stays; cues play over it. */
}

static BlastSlot *grab_slot(void)
{
    unsigned i;
    BlastSlot *candidate;
    for (i = 0; i < BLAST_SLOTS; ++i) {
        candidate = &slots[slot_cursor];
        slot_cursor = (slot_cursor + 1u) % BLAST_SLOTS;
        if (!candidate->active) return candidate;
    }
    return &slots[0];   /* all busy: reuse the oldest */
}

static void start_blast(const MercFxImpact *event, int wide, int tsar, float duration)
{
    BlastSlot *slot = grab_slot();
    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    slot->wide = wide;
    slot->tsar = tsar;
    slot->age = 0.0f;
    slot->duration = duration;
    slot->pos[0] = event->position[0];
    slot->pos[1] = event->position[1];
    slot->pos[2] = event->position[2];
    slot->next_cap = 0.0f;
    slot->next_stem = 0.05f;
    slot->next_wave = 0.02f;
    slot->next_demo = 0.25f;
    slot->next_ring = 2.0f;
    slot->next_scorch = 0.3f;
    slot->rise = (cfg_audio != 0) ? sound_launch : 0;
    slot->shock = (cfg_audio == 1) ? sound_shock : 0;

    /* retail Meltdown visual fired from world_tick */
    memcpy(pending_explosion_pos, event->position, sizeof(pending_explosion_pos));
    pending_explosion_radius = 1100.0f;
    pending_explosion = 1;

    /* the whiteout: stacked flashes, longer at MEGA/TSAR */
    flash_hold = wide ? 1.2f : 0.8f;
    flash_fade = wide ? 3.0f : 2.0f;
    flash_time = 0.0f;
}

static MercFxDecision __cdecl tw_impact(const MercFxImpact *event, void *userdata)
{
    MercFxImpact *ev = (MercFxImpact *)event;   /* framework-owned copy */
    MercFxBlastOptions options;
    uint32_t first_cue;
    unsigned i, cat = 0xFFu;
    int known = 0;

    (void)userdata;
    if (!cfg_master) return MERCFX_UNHANDLED;

    for (i = 0; i < (unsigned)TRIGGER_TEMPLATE_COUNT; ++i)
        if (event->template_hash == template_hash[i] && bound_flags[i]) {
            cat = trigger_templates[i].cat; known = 1; break;
        }
    if (!known) {
        /* Catch-all weapon enhancement: unknown explosive impacts. */
        if (cfg_realism && event->splash_radius >= 25.0f) {
            ev->splash_damage *= 1.6f;
            ev->splash_radius *= 1.4f;
            LOG("weapon impact hash=%08X r=%.1f enhanced",
                event->template_hash, event->splash_radius);
            return MERCFX_KEEP;
        }
        return MERCFX_UNHANDLED;
    }
    if (!cat_allows(cat)) return MERCFX_UNHANDLED;

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.scorch_radius = 40.0f;
    options.suppress_default_sound = 0u;
    audio_opts(&options, &first_cue);

    switch (cat) {
    case CAT_BB:
        if (cfg_bb == 1) {   /* ENHANCED */
            ev->splash_damage *= 3.0f;
            ev->splash_force *= 3.0f;
            ev->splash_radius *= 2.0f;
            if (ev->splash_radius > 500.0f) ev->splash_radius = 500.0f;
            options.damage_type = MERCFX_BLAST_BUNKERBUSTER;
            options.shake_duration = 4.0f; options.shake_magnitude = 3.0f;
        } else {             /* NUKE */
            ev->splash_damage *= 10.0f;
            ev->splash_force *= 12.0f;
            ev->splash_radius = 1000.0f;          /* 1K city radius */
            options.damage_type = MERCFX_BLAST_BUNKERBUSTER;
            quake_opts(&options);
            if (!cfg_effect) {                    /* CLASSIC shake only */
                options.shake_duration = 6.0f; options.shake_magnitude = 4.0f;
            }
            start_blast(event, cfg_strategic == 3, cfg_strategic == 2,
                        cfg_strategic == 3 ? 45.0f : 35.0f);
            if (cfg_strategic == 0) { /* strategic OFF: no cloud, still the blast */ }
        }
        break;
    case CAT_CRUISE:
        if (cfg_foab == 1) {  /* ENHANCED */
            ev->splash_damage *= 2.5f;
            ev->splash_force *= 2.5f;
            ev->splash_radius *= 2.0f;
            if (ev->splash_radius > 450.0f) ev->splash_radius = 450.0f;
            options.damage_type = MERCFX_BLAST_FROM_PROJECTILE;
            options.shake_duration = 3.0f; options.shake_magnitude = 2.0f;
        } else {              /* FOAB / MEGA */
            float mult = (cfg_foab == 3) ? 2.0f : 1.0f;
            ev->splash_damage *= 6.0f * mult;
            ev->splash_force *= 8.0f * mult;
            ev->splash_radius = (cfg_foab == 3) ? 1000.0f : 700.0f;
            options.damage_type = MERCFX_BLAST_BUNKERBUSTER;
            quake_opts(&options);
            start_blast(event, cfg_foab == 3, 0,
                        (cfg_foab == 3) ? 35.0f : 25.0f);
        }
        break;
    case CAT_SMART:
        ev->splash_damage *= (cfg_surgical && event->template_hash ==
                              fnv1a_casefold("template_amm_surgicalstrike")) ? 3.0f : 2.0f;
        ev->splash_force *= 2.0f;
        ev->splash_radius *= 2.0f;
        if (ev->splash_radius > 400.0f) ev->splash_radius = 400.0f;
        options.damage_type = MERCFX_BLAST_BUNKERBUSTER;
        options.shake_duration = 2.0f; options.shake_magnitude = 1.5f;
        break;
    case CAT_CARPET:
        ev->splash_damage *= 2.5f;
        ev->splash_force *= 2.5f;
        ev->splash_radius *= 2.5f;
        if (ev->splash_radius > 350.0f) ev->splash_radius = 350.0f;
        options.damage_type = MERCFX_BLAST_STANDARD;
        options.shake_duration = 2.5f; options.shake_magnitude = 2.0f;
        break;
    case CAT_C4:
        if (cfg_c4 >= 2u) {
            float mult = (cfg_c4 == 3u) ? 2.0f : 1.0f;
            ev->splash_damage *= 6.0f * mult;
            ev->splash_force *= 5.0f * mult;
            ev->splash_radius = (cfg_c4 == 3u) ? 160.0f : 120.0f;
            options.damage_type = MERCFX_BLAST_C4;    /* grey blast, no red streaks */
            options.shake_duration = 2.0f * mult; options.shake_magnitude = 2.0f;
        } else if (cfg_c4 == 1u) {
            ev->splash_damage *= 2.0f;
            ev->splash_radius *= 1.5f;
            options.damage_type = MERCFX_BLAST_C4;
        }
        break;
    default:   /* CAT_WEAPON */
        ev->splash_damage *= 1.8f;
        ev->splash_force *= 1.6f;
        ev->splash_radius *= 1.5f;
        options.damage_type = MERCFX_BLAST_FROM_PROJECTILE;
        break;
    }

    if (!fx->apply_blast_configured(event, &options)) {
        pending_log_blast_failure = 1;
        return MERCFX_UNHANDLED;
    }

    if (cat == CAT_BB && cfg_bb == 2)
        LOG("Meltdown nuke effect fired r=%.0f", pending_explosion_radius);
    DLOG("impact cat=%u r=%.1f handled", cat, event->splash_radius);

    /* slow motion on nuke-tier strikes */
    if ((cat == CAT_BB && cfg_bb == 2) || (cat == CAT_CRUISE && cfg_foab >= 2))
        fx->request_slowdown(NUKE_OWNER_KEY, 0.45f, 1.5f);

    /* layered audio: launch roar + primary + shockwave */
    if (cfg_audio != 0) {
        if (sound_launch && fx->sound_ready(sound_launch))
            fx->play_sound(sound_launch, event->position);
        if (first_cue && fx->sound_ready(first_cue))
            fx->play_sound(first_cue, event->position);
        else if (sound_fae && fx->sound_ready(sound_fae))
            fx->play_sound(sound_fae, event->position);
    }

    if (cat == CAT_BB && cfg_bb == 2 && cfg_strategic > 0)
        return MERCFX_REMOVE;
    if (cat == CAT_CRUISE && cfg_foab >= 2)
        return MERCFX_REMOVE;
    return MERCFX_KEEP;
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
    if (!n || n > FLASH_MAX_BRUSHES) return;

    if (t < flash_hold) {
        alpha = 1.0f;
    } else {
        float total = t - flash_hold;
        if (total >= flash_fade) { flash_time = -1.0f; return; }
        alpha = powf(1.0f - total / flash_fade, 1.6f);
    }
    /* 8 stacked screen-filling passes */
    per_brush = 1.0f - powf(1.0f - alpha / (float)FLASH_PASSES, 1.0f / (float)n);
    if (per_brush < 0.0f) per_brush = 0.0f;
    color = ((uint32_t)(per_brush * 127.0f + 0.5f) << 24) | 0x00FFFFFFu;

    begin_args[0] = flash_texture;
    begin_args[1] = color;
    begin_args[2] = 0u;
    begin_args[3] = 0u;
    begin_args[4] = 0u;
    host->guest_call(SPRITE_BEGIN_FN, brush, 5u, begin_args);

    quad_args[0] = float_bits(0.0f);
    quad_args[1] = float_bits(0.0f);
    quad_args[2] = float_bits(640.0f);
    quad_args[3] = float_bits(480.0f);
    quad_args[4] = float_bits(0.0f);
    quad_args[5] = float_bits(0.0f);
    quad_args[6] = float_bits(1.0f);
    quad_args[7] = float_bits(1.0f);
    host->guest_call(SPRITE_QUAD_FN, brush, 8u, quad_args);
    for (pass = 1; pass < FLASH_PASSES; ++pass) {
        host->guest_call(SPRITE_BEGIN_FN, brush, 5u, begin_args);
        host->guest_call(SPRITE_QUAD_FN, brush, 8u, quad_args);
    }
}

/* Wave demolition: retail explosions crumble buildings at the wavefront. */
static void wave_demolition(void)
{
    unsigned i;
    float angle, terrain = 0.0f;
    uint32_t xp[13];

    for (i = 0; i < BLAST_SLOTS; ++i) {
        BlastSlot *slot = &slots[i];
        if (!slot->active || !cfg_demo) continue;
        slot->next_demo -= 0.016f;   /* approximated per world_tick */
        if (slot->next_demo > 0.0f) continue;
        slot->next_demo = 0.25f;
        if (slot->strikes >= EXTRA_QUEUE) continue;
        angle = 6.2832f * (float)(slot->demo_angle % 24u) / 24.0f;
        ++slot->demo_angle;
        ++slot->strikes;
        {
            float radius = 30.0f + slot->age * 140.0f;
            float x = slot->pos[0] + cosf(angle) * radius;
            float z = slot->pos[2] + sinf(angle) * radius;
            if (fx && fx->sample_terrain_height && fx->sample_terrain_height(x, z, &terrain))
                ; /* terrain sampled */
            memcpy(xp, slot->pos, 12);
            xp[0] = float_bits(x);
            xp[2] = float_bits(z);
            xp[1] = float_bits(terrain > 0.0f ? terrain : slot->pos[1]);
            xp[3] = float_bits(70.0f);    /* r=70 demolition */
            xp[4] = float_bits(0.0f);     /* force 0 */
            xp[5] = 0; xp[6] = 0;         /* damage 0 (visual + engine demolition) */
            xp[7] = meltdown_visual;
            xp[8] = 0; xp[9] = 0; xp[10] = 0; xp[11] = 0; xp[12] = 0;
            if (have_explosion_fn && host->guest_call(EXPLOSION_FN, 0, 13, xp))
                DLOG("wave demolition strike r=%.0f", 70.0f);
        }
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

    if (attach_state == 2 && bound_count < (unsigned)TRIGGER_TEMPLATE_COUNT) {
        if (next_rebind_time == 0.0) {
            next_rebind_time = world_time + 2.0;
        } else if (world_time >= next_rebind_time) {
            next_rebind_time = world_time + 2.0;
            bind_pass(0);
            if (bound_count == (unsigned)TRIGGER_TEMPLATE_COUNT)
                LOG("all %u trigger templates bound after retries", bound_count);
        }
    }

    if (pending_log_blast_failure) {
        pending_log_blast_failure = 0;
        LOG("blast rejected by the framework; the vanilla explosion took over");
    } else if (pending_log_trigger[0]) {
        LOG("total warfare detonated on %s", pending_log_trigger);
        pending_log_trigger[0] = 0;
    }

    /* Retail Meltdown visual explosion (visual only, zero damage). */
    if (pending_explosion) {
        pending_explosion = 0;
        if (have_explosion_fn && cfg_master) {
            uint32_t xp[13];
            memcpy(xp, pending_explosion_pos, 12);
            xp[3] = float_bits(pending_explosion_radius);
            xp[4] = float_bits(0.0f);
            xp[5] = 0; xp[6] = 0;
            xp[7] = meltdown_visual;
            xp[8] = 0; xp[9] = 0; xp[10] = 0; xp[11] = 0; xp[12] = 0;
            host->guest_call(EXPLOSION_FN, 0, 13, xp);
            LOG("Meltdown nuke effect fired r=%.0f", pending_explosion_radius);
        }
    }

    wave_demolition();

    if (flash_time >= 0.0f) {
        flash_time += delta;
        if (flash_time > flash_hold + flash_fade + 1.0f)
            flash_time = -1.0f;
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
    if (fx && fx->cancel_slowdown) fx->cancel_slowdown(NUKE_OWNER_KEY);
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
    "BUILT ON SVERED'S MERCFX",
    "Total warfare: every heavy bomb becomes a Mercenaries 2 / World in Conflict style "
    "nuke - flat-topped orange fire mushroom, red-brown smoke, low ground fire wave, "
    "wave demolition, 1K city blast radius, Meltdown blast, whiteout flash, layered "
    "audio and earthquake. Needs Native Effects.",
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

    cfg_master = host->read_setting(MOD_INI, MOD_INI_SECTION, "Master", cfg_master) & 1u;
    cfg_smart = host->read_setting(MOD_INI, MOD_INI_SECTION, "SmartBomb", cfg_smart) & 1u;
    cfg_surgical = host->read_setting(MOD_INI, MOD_INI_SECTION, "SurgicalStrike", cfg_surgical) & 1u;
    cfg_carpet = host->read_setting(MOD_INI, MOD_INI_SECTION, "CarpetBomb", cfg_carpet) & 1u;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "BunkerBuster", cfg_bb);
    if (v > 2u) v = 2u; cfg_bb = v;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "CruiseFOAB", cfg_foab);
    if (v > 3u) v = 3u; cfg_foab = v;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "StrategicNuke", cfg_strategic);
    if (v > 3u) v = 3u; cfg_strategic = v;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "C4Mega", cfg_c4);
    if (v > 3u) v = 3u; cfg_c4 = v;
    cfg_realism = host->read_setting(MOD_INI, MOD_INI_SECTION, "WeaponRealism", cfg_realism) & 1u;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "Earthquake", cfg_quake);
    if (v > 2u) v = 2u; cfg_quake = v;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "NukeAudio", cfg_audio);
    if (v > 2u) v = 2u; cfg_audio = v;
    cfg_demo = host->read_setting(MOD_INI, MOD_INI_SECTION, "WaveDemo", cfg_demo) & 1u;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "NukeEffect", cfg_effect);
    if (v > 1u) v = 1u; cfg_effect = v;
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "ShopProfile", cfg_shop);
    if (v > 2u) v = 2u; cfg_shop = v;
    cfg_debug = host->read_setting(MOD_INI, MOD_INI_SECTION, "DebugLog", cfg_debug) & 1u;
    /* ini-only option: the MODS page is full at 15 settings */
    v = host->read_setting(MOD_INI, MOD_INI_SECTION, "ViewRange", (unsigned)view_range);
    if (v < 128u) v = 128u;
    if (v > 4096u) v = 4096u;
    view_range = (float)v;

    flash_texture = host->register_texture(4, 4, flash_pixels);

    register_shop_item();
    try_attach();

    return &descriptor;
}

/* Local-build shims: no CRT startup object is linked. */
const float _fltused = 1.0f;
BOOL WINAPI _DllMainCRTStartup(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
