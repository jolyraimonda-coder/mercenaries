/* SPDX-License-Identifier: MIT */
/*
 * Total Warfare Overhaul
 * Mercenaries: Playground of Destruction (Xbox) - Recompiled 1.1.1
 * Mod Manager / Mod API 2 DLL mod, with optional MercFx (Native Effects) integration.
 *
 * Build (64-bit Windows DLL), with mercenaries_mod.h and mercfx.h beside this file:
 *   cl /nologo /W4 /O2 /LD total_warfare.c /I . /Fe:total_warfare.dll /link /MACHINE:X64
 *   x86_64-w64-mingw32-gcc -shared -O2 -Wall -Wextra -I. total_warfare.c -o total_warfare.dll
 *   clang --target=x86_64-pc-windows-msvc -shared -O2 -I. total_warfare.c -o total_warfare.dll
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "mercenaries_mod.h"
#include "mercfx.h"

#define MOD_NAME      "TOTAL WARFARE OVERHAUL"
#define MOD_VERSION   "2.4.0"
#define MOD_CREDITS   "BY GLYPH CLUSTER"
#define SETTINGS_FILE "mods\\total_warfare.ini"
#define SECTION       "TotalWarfare"
#define FX_EFFECT     "totalwarfare.fx"
#define FX_OWNER      "totalwarfare"
#define EXPLOSION_FN  0x000B6460u

enum {
    SET_MASTER = 0,
    SET_SMART_BOMB,
    SET_SURGICAL,
    SET_CARPET,
    SET_BUNKER_NUKE,
    SET_CRUISE_FOAB,
    SET_NUKE_EFFECT,
    SET_STRATEGIC_NUKE,
    SET_C4_MEGA,
    SET_WEAPON_REALISM,
    SET_EARTHQUAKE,
    SET_NUKE_AUDIO,
    SET_WAVE_DEMO,
    SET_SHOP_PROFILE,
    SET_DEBUG_LOG,
    SETTING_COUNT
};

enum {
    CAT_SMART = 0,
    CAT_SURGICAL,
    CAT_CARPET,
    CAT_BUNKER,
    CAT_CRUISE,
    CAT_STRATEGIC,
    CAT_C4,
    CAT_WEAPON,
    CAT_COUNT
};

typedef struct { const char *name; unsigned char category; } TriggerTemplate;

static const TriggerTemplate kTriggers[] = {
    /* Smart / precision bombs */
    {"template_amm_smartbomb",        CAT_SMART},
    {"template_amm_smartbomb0",       CAT_SMART},
    {"template_amm_smartbomb2",       CAT_SMART},
    {"template_amm_smartbomb3",       CAT_SMART},
    {"template_amm_hvysmartbomb",     CAT_SMART},
    {"template_amm_hvysmartbomb0",    CAT_SMART},
    /* Surgical strike */
    {"template_amm_surgicalstrike",   CAT_SURGICAL},
    {"template_amm_surgicalstrike0",  CAT_SURGICAL},
    /* Carpet / cluster / area bombardment */
    {"template_amm_carpetbomb",       CAT_CARPET},
    {"template_amm_carpetbomb0",      CAT_CARPET},
    {"template_amm_carpetbomblet",    CAT_CARPET},
    {"template_amm_clusterbomb",      CAT_CARPET},
    {"template_amm_clusterbomb0",     CAT_CARPET},
    {"template_amm_clusterbombBomb",  CAT_CARPET},
    {"template_amm_clusterbomblet",   CAT_CARPET},
    {"template_amm_clusterbomblet0",  CAT_CARPET},
    {"template_amm_fuelairbomb",      CAT_CRUISE},
    {"template_amm_fuelairbomb0",     CAT_CRUISE},
    {"template_amm_artillery",        CAT_CARPET},
    {"template_amm_artillery0",       CAT_CARPET},
    {"template_amm_artillery2",       CAT_CARPET},
    {"template_amm_artillery3",       CAT_CARPET},
    /* Bunker buster */
    {"template_amm_bunkerbuster",     CAT_BUNKER},
    {"template_amm_bunkerbuster0",    CAT_BUNKER},
    {"template_amm_bunkerbusterbomb", CAT_BUNKER},
    /* Cruise / heavy missile */
    {"template_amm_cruisemissile",    CAT_CRUISE},
    {"template_amm_cruisemissile0",   CAT_CRUISE},
    {"template_amm_airsuperiority",   CAT_CRUISE},
    {"template_amm_airsuperiority0",  CAT_CRUISE},
    /* Tank buster / guided anti-armor */
    {"template_amm_tankbuster",       CAT_WEAPON},
    {"template_amm_tankbuster0",      CAT_WEAPON},
    {"template_amm_guidedmissile",    CAT_WEAPON},
    {"template_amm_guidedmissile0",   CAT_WEAPON},
    /* Strategic / nuclear */
    {"template_amm_strategicmissile", CAT_STRATEGIC},
    {"template_amm_strategicmissile0",CAT_STRATEGIC},
    {"template_amm_meltdown",         CAT_STRATEGIC},
    /* C4 / placed explosives (best-effort if MercFx reports impacts) */
    {"template_amm_c4",               CAT_C4},
    {"template_amm_c4explosive",      CAT_C4},
    {"template_amm_c4_0",             CAT_C4},
    {"template_amm_satchel",          CAT_C4},
    /* Weapons / other explosives (best-effort) */
    {"template_amm_grenade",          CAT_WEAPON},
    {"template_amm_fraggrenade",      CAT_WEAPON},
    {"template_amm_rpg",              CAT_WEAPON},
    {"template_amm_rpg7",             CAT_WEAPON},
    {"template_amm_rpgrocket",        CAT_WEAPON},
    {"template_amm_law",              CAT_WEAPON},
    {"template_amm_lawrocket",        CAT_WEAPON},
    {"template_amm_at4",              CAT_WEAPON},
    {"template_amm_atgm",             CAT_WEAPON},
    {"template_amm_tow",              CAT_WEAPON},
    {"template_amm_towmissile",       CAT_WEAPON},
    {"template_amm_hellfire",         CAT_WEAPON},
    {"template_amm_hellfirem",        CAT_WEAPON},
    {"template_amm_hellfirem0",       CAT_WEAPON},
    {"template_amm_stinger",          CAT_WEAPON},
    {"template_amm_missile",          CAT_WEAPON},
    {"template_amm_tankround",        CAT_WEAPON},
    {"template_amm_tankshell",        CAT_WEAPON},
    {"template_amm_tankcannon",       CAT_WEAPON},
    {"template_amm_105mm",            CAT_WEAPON},
    {"template_amm_120mm",            CAT_WEAPON},
    {"template_amm_125mm",            CAT_WEAPON},
    {"template_amm_30mm",             CAT_WEAPON},
    {"template_amm_20mm",             CAT_WEAPON},
    {"template_amm_40mm",             CAT_WEAPON},
    {"template_amm_cannon",           CAT_WEAPON},
    {"template_amm_mortar",           CAT_WEAPON},
    {"template_amm_mortarshell",      CAT_WEAPON},
    {"template_amm_m67",              CAT_WEAPON},
    {"template_amm_grenadefrag",      CAT_WEAPON},
};
#define TRIGGER_COUNT (sizeof(kTriggers)/sizeof(kTriggers[0]))

static const MercenariesModHost *g_host;
static unsigned g_settings[SETTING_COUNT];
static unsigned g_ticks;
static unsigned g_announced;
static unsigned g_shop_attempted;

static const MercFxApi *g_fx;
static int g_fx_state; /* 0 try, 1 pending, 2 attached, 3 unavailable */
static unsigned g_fx_misses;
static unsigned g_fx_attempts;
static uint32_t g_template_hash[TRIGGER_COUNT];
static unsigned char g_bound[TRIGGER_COUNT];
static unsigned g_bound_count;
static double g_world_time;
static double g_next_rebind;
static int g_pending_meltdown;
static float g_pending_pos[3];
static float g_pending_meltdown_r = 500.0f;
static uint32_t g_meltdown_hash;
static int g_have_explosion_fn;
static int g_checked_retail;
static int g_pending_log_fail;
static int g_nuke_audio_pending;
static char g_pending_log_tpl[64];

/* Rolling blast timeline + queued staged mushroom visuals (fired from world_tick). */
typedef struct {
    int active;
    int wide;
    float pos[3];
    float age;
    float duration;
    float next_ring;
    float next_scorch;
    unsigned scorch_step;
    unsigned meltdown_stage;
    unsigned tsar;
    unsigned tsar_sample;
    float tsar_pending;
} BlastSlot;
#define BLAST_SLOTS 3
static BlastSlot g_slots[BLAST_SLOTS];
static unsigned g_slot_cursor;
static int g_sim_registered;

#define EXTRA_QUEUE 32
static float g_extra_pos[EXTRA_QUEUE][3];
static float g_extra_radius[EXTRA_QUEUE];
static unsigned g_extra_head;
static unsigned g_extra_count;

static const char *const on_off[] = {"OFF", "ON"};
static const char *const bunker_values[] = {"OFF", "HEAVY", "NUKE"};
static const char *const cruise_values[] = {"OFF", "HEAVY", "THERMO", "FOAB"};
static const char *const strategic_values[] = {"OFF", "ON", "TSAR", "MEGA"};
static const char *const nukefx_values[] = {"CLASSIC", "MELTDOWN"};
static const char *const c4_values[] = {"OFF", "MEGA", "MEGA 1KM", "MEGA 2KM"};
static const char *const quake_values[] = {"OFF", "STRONG", "EXTREME"};
static const char *const audio_values[] = {"OFF", "LAYERED", "NUKE ONLY"};
static const char *const shop_values[] = {"OFF", "ON", "MAX"};

static const unsigned setting_defaults[SETTING_COUNT] = {
    1u, 1u, 1u, 1u, 2u, 3u, 1u, 1u, 3u, 1u, 2u, 2u, 1u, 2u, 0u
};

static unsigned clamp_value(unsigned value, unsigned count)
{
    return value < count ? value : (count ? count - 1u : 0u);
}

static void save_setting(const char *key, unsigned value)
{
    if (g_host) g_host->write_setting(SETTINGS_FILE, SECTION, key, value);
}

static unsigned get_master(void) { return g_settings[SET_MASTER]; }
static void set_master(unsigned v) { g_settings[SET_MASTER] = clamp_value(v, 2u); save_setting("Master", g_settings[SET_MASTER]); }
static unsigned get_smart(void) { return g_settings[SET_SMART_BOMB]; }
static void set_smart(unsigned v) { g_settings[SET_SMART_BOMB] = clamp_value(v, 2u); save_setting("SmartBomb", g_settings[SET_SMART_BOMB]); }
static unsigned get_surgical(void) { return g_settings[SET_SURGICAL]; }
static void set_surgical(unsigned v) { g_settings[SET_SURGICAL] = clamp_value(v, 2u); save_setting("SurgicalStrike", g_settings[SET_SURGICAL]); }
static unsigned get_carpet(void) { return g_settings[SET_CARPET]; }
static void set_carpet(unsigned v) { g_settings[SET_CARPET] = clamp_value(v, 2u); save_setting("CarpetBomb", g_settings[SET_CARPET]); }
static unsigned get_bunker(void) { return g_settings[SET_BUNKER_NUKE]; }
static void set_bunker(unsigned v) { g_settings[SET_BUNKER_NUKE] = clamp_value(v, 3u); save_setting("BunkerBuster", g_settings[SET_BUNKER_NUKE]); }
static unsigned get_cruise(void) { return g_settings[SET_CRUISE_FOAB]; }
static void set_cruise(unsigned v) { g_settings[SET_CRUISE_FOAB] = clamp_value(v, 4u); save_setting("CruiseFOAB", g_settings[SET_CRUISE_FOAB]); }
static unsigned get_nukefx(void) { return g_settings[SET_NUKE_EFFECT]; }
static void set_nukefx(unsigned v) { g_settings[SET_NUKE_EFFECT] = clamp_value(v, 2u); save_setting("NukeEffect", g_settings[SET_NUKE_EFFECT]); }
static unsigned get_strategic(void) { return g_settings[SET_STRATEGIC_NUKE]; }
static void set_strategic(unsigned v) { g_settings[SET_STRATEGIC_NUKE] = clamp_value(v, 4u); save_setting("StrategicNuke", g_settings[SET_STRATEGIC_NUKE]); }
static unsigned get_c4(void) { return g_settings[SET_C4_MEGA]; }
static void set_c4(unsigned v) { g_settings[SET_C4_MEGA] = clamp_value(v, 4u); save_setting("C4Mega", g_settings[SET_C4_MEGA]); }
static unsigned get_realism(void) { return g_settings[SET_WEAPON_REALISM]; }
static void set_realism(unsigned v) { g_settings[SET_WEAPON_REALISM] = clamp_value(v, 2u); save_setting("WeaponRealism", g_settings[SET_WEAPON_REALISM]); }
static unsigned get_quake(void) { return g_settings[SET_EARTHQUAKE]; }
static void set_quake(unsigned v) { g_settings[SET_EARTHQUAKE] = clamp_value(v, 3u); save_setting("Earthquake", g_settings[SET_EARTHQUAKE]); }
static unsigned get_audio(void) { return g_settings[SET_NUKE_AUDIO]; }
static void set_audio(unsigned v) { g_settings[SET_NUKE_AUDIO] = clamp_value(v, 3u); save_setting("NukeAudio", g_settings[SET_NUKE_AUDIO]); }
static unsigned get_wavedemo(void) { return g_settings[SET_WAVE_DEMO]; }
static void set_wavedemo(unsigned v) { g_settings[SET_WAVE_DEMO] = clamp_value(v, 2u); save_setting("WaveDemo", g_settings[SET_WAVE_DEMO]); }
static unsigned get_shop(void) { return g_settings[SET_SHOP_PROFILE]; }
static void set_shop(unsigned v) { g_settings[SET_SHOP_PROFILE] = clamp_value(v, 3u); save_setting("ShopProfile", g_settings[SET_SHOP_PROFILE]); }
static unsigned get_debug(void) { return g_settings[SET_DEBUG_LOG]; }
static void set_debug(unsigned v) { g_settings[SET_DEBUG_LOG] = clamp_value(v, 2u); save_setting("DebugLog", g_settings[SET_DEBUG_LOG]); }

static const MercenariesModSetting settings[SETTING_COUNT] = {
    {"ENABLED", on_off, 2u, get_master, set_master},
    {"SMART BOMB", on_off, 2u, get_smart, set_smart},
    {"SURGICAL STRIKE", on_off, 2u, get_surgical, set_surgical},
    {"CARPET BOMB", on_off, 2u, get_carpet, set_carpet},
    {"BUNKER BUSTER", bunker_values, 3u, get_bunker, set_bunker},
    {"CRUISE MISSILE", cruise_values, 4u, get_cruise, set_cruise},
    {"NUKE EFFECT", nukefx_values, 2u, get_nukefx, set_nukefx},
    {"STRATEGIC NUKE", strategic_values, 4u, get_strategic, set_strategic},
    {"C4 MEGA BLAST", c4_values, 4u, get_c4, set_c4},
    {"WEAPON REALISM", on_off, 2u, get_realism, set_realism},
    {"EARTHQUAKE", quake_values, 3u, get_quake, set_quake},
    {"NUKE AUDIO", audio_values, 3u, get_audio, set_audio},
    {"WAVE DEMOLITION", on_off, 2u, get_wavedemo, set_wavedemo},
    {"SHOP PROFILE", shop_values, 3u, get_shop, set_shop},
    {"DEBUG LOG", on_off, 2u, get_debug, set_debug},
};

static const char *const setting_descriptions[SETTING_COUNT] = {
    "Master switch for Total Warfare Overhaul profiles and logging.",
    "Smart bomb: boosts blast via Native Effects when available.",
    "Surgical strike: boosts blast via Native Effects when available.",
    "Carpet bomb: boosts bomblet blasts via Native Effects when available.",
    "Bunker buster: HEAVY/NUKE blast, shockwave and meltdown FX with Native Effects.",
    "Cruise missile: HEAVY/THERMO/FOAB blast and FX with Native Effects.",
    "Nuke blast look: MELTDOWN swaps the plain blast for the retail meltdown explosion at impact.",
    "Strategic missile: ON = strong blast; TSAR = 60s mushroom cloud; MEGA = 120s cloud + city dust blanket.",
    "C4 profile: enhances blast on C4 impact if MercFx sees it; detonator range needs runtime hooks.",
    "Weapon realism: enhances explosive weapon impacts via MercFx when possible; ballistics/tracers need hooks/data.",
    "Ground shake on nuke blasts: STRONG = 10s rolling, EXTREME = 20s violent.",
    "Nuke blast sound: LAYERED = cue + built-in explosion, NUKE ONLY = cue alone.",
    "The grey wave demolishes buildings as it sweeps past (retail demolition calls).",
    "Register mod strings and attempt extra shop/catalog hooks if possible.",
    "Writes status and heartbeat lines to the Mod Manager log.",
};

static const uint8_t setting_controls[SETTING_COUNT] = {
    MERCENARIES_MOD_CONTROL_PICKER, MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_PICKER, MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL, MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL,
    MERCENARIES_MOD_CONTROL_PICKER, MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL,
    MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL, MERCENARIES_MOD_CONTROL_PICKER,
    MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL, MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL,
    MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL, MERCENARIES_MOD_CONTROL_SLIDER_VALUE_IN_LABEL,
    MERCENARIES_MOD_CONTROL_PICKER,
};

static uint32_t fnv1a(const char *text)
{
    uint32_t hash = 0x811C9DC5u;
    while (*text) hash = (hash ^ ((unsigned char)*text++ | 0x20u)) * 16777619u;
    return hash;
}

static uint32_t fbits(float v) { uint32_t b; memcpy(&b, &v, sizeof(b)); return b; }

#define LOGF(...) do { if (g_host && g_host->log) g_host->log("totalwarfare", __VA_ARGS__); } while (0)

static void load_settings(void)
{
    if (!g_host) return;
    g_settings[SET_MASTER]         = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "Master", setting_defaults[SET_MASTER]), 2u);
    g_settings[SET_SMART_BOMB]     = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "SmartBomb", setting_defaults[SET_SMART_BOMB]), 2u);
    g_settings[SET_SURGICAL]       = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "SurgicalStrike", setting_defaults[SET_SURGICAL]), 2u);
    g_settings[SET_CARPET]         = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "CarpetBomb", setting_defaults[SET_CARPET]), 2u);
    g_settings[SET_BUNKER_NUKE]    = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "BunkerBuster", setting_defaults[SET_BUNKER_NUKE]), 3u);
    g_settings[SET_CRUISE_FOAB]    = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "CruiseFOAB", setting_defaults[SET_CRUISE_FOAB]), 4u);
    g_settings[SET_NUKE_EFFECT]    = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "NukeEffect", setting_defaults[SET_NUKE_EFFECT]), 2u);
    g_settings[SET_STRATEGIC_NUKE] = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "StrategicNuke", setting_defaults[SET_STRATEGIC_NUKE]), 4u);
    g_settings[SET_C4_MEGA]        = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "C4Mega", setting_defaults[SET_C4_MEGA]), 4u);
    g_settings[SET_WEAPON_REALISM] = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "WeaponRealism", setting_defaults[SET_WEAPON_REALISM]), 2u);
    g_settings[SET_EARTHQUAKE]     = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "Earthquake", setting_defaults[SET_EARTHQUAKE]), 3u);
    g_settings[SET_NUKE_AUDIO]     = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "NukeAudio", setting_defaults[SET_NUKE_AUDIO]), 3u);
    g_settings[SET_WAVE_DEMO]      = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "WaveDemo", setting_defaults[SET_WAVE_DEMO]), 2u);
    g_settings[SET_SHOP_PROFILE]   = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "ShopProfile", setting_defaults[SET_SHOP_PROFILE]), 3u);
    g_settings[SET_DEBUG_LOG]      = clamp_value(g_host->read_setting(SETTINGS_FILE, SECTION, "DebugLog", setting_defaults[SET_DEBUG_LOG]), 2u);
}

static unsigned category_enabled(unsigned cat)
{
    if (!g_settings[SET_MASTER]) return 0;
    switch (cat) {
        case CAT_SMART: return g_settings[SET_SMART_BOMB];
        case CAT_SURGICAL: return g_settings[SET_SURGICAL];
        case CAT_CARPET: return g_settings[SET_CARPET];
        case CAT_BUNKER: return g_settings[SET_BUNKER_NUKE] != 0u;
        case CAT_CRUISE: return g_settings[SET_CRUISE_FOAB] != 0u;
        case CAT_STRATEGIC: return g_settings[SET_STRATEGIC_NUKE];
        case CAT_C4: return g_settings[SET_C4_MEGA] != 0u;
        case CAT_WEAPON: return g_settings[SET_WEAPON_REALISM];
        default: return 0;
    }
}

static MercFxDecision __cdecl fx_impact(const MercFxImpact *event, void *userdata);

static void queue_meltdown(const float pos[3], float radius)
{
    unsigned idx;
    if (g_extra_count >= EXTRA_QUEUE) return;
    idx = (g_extra_head + g_extra_count) % EXTRA_QUEUE;
    memcpy(g_extra_pos[idx], pos, sizeof(g_extra_pos[idx]));
    g_extra_radius[idx] = radius;
    ++g_extra_count;
}

/* ---- TSAR BOMBA mushroom cloud (particle visuals via Native Effects v1) ----
   Modeled on the MOAB native-effect pattern: register custom sprite
   descriptors, then emit continuously from the simulation tick. One particle
   lives at most ~30s, so the cloud stays alive by re-emitting for the whole
   timeline (60s TSAR / 120s MEGA). */
enum { TSAR_FLASH, TSAR_FIRE, TSAR_DUST, TSAR_SMOKE, TSAR_PLUME, TSAR_GROUND, TSAR_WAVE, TSAR_PARTICLES };
#define TSAR_TAU 6.28318530718f
#define TSAR_GOLDEN 2.39996322973f
static uint32_t g_tsar_particles[TSAR_PARTICLES];
static uint32_t g_nuke_sound;
static float g_tsar_view = 4096.0f;
static int g_tsar_ready;
static uint32_t g_tsar_rand = 0x74736172u;

static float tsar_rand_unit(void)
{
    uint32_t v = g_tsar_rand;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    g_tsar_rand = v;
    return (float)(v >> 8) * (1.0f / 16777216.0f);
}

static void emit_tsar(unsigned kind, const float p[3], const float vel[3])
{
    if (!g_fx || !g_fx->emit_particle || !g_tsar_ready) return;
    g_fx->emit_particle(g_tsar_particles[kind], p, vel, (tsar_rand_unit() - 0.5f) * 1.5f, 1.0f);
}

static void tsar_burst(BlastSlot *s)
{
    /* MOAB-style opening burst (Svered's authoring), scaled up for nuke tier */
    unsigned i;
    float p[3], vel[3], angle, speed;
    p[0] = s->pos[0]; p[1] = s->pos[1] + 8.0f; p[2] = s->pos[2];
    vel[0] = vel[1] = vel[2] = 0.0f;
    { unsigned f; for (f = 0; f < 8; ++f) { p[1] = s->pos[1] + 10.0f + 12.0f * (float)f; emit_tsar(TSAR_FLASH, p, vel); } }
    p[1] = s->pos[1] + 8.0f;
    /* compact fire core: orange stays at the fireball, not the wave */
    for (i = 0; i < 12; ++i) {
        angle = TSAR_TAU * tsar_rand_unit();
        speed = 8.0f + 12.0f * tsar_rand_unit();
        vel[0] = cosf(angle) * speed; vel[1] = 3.0f * tsar_rand_unit(); vel[2] = sinf(angle) * speed;
        emit_tsar(TSAR_FIRE, p, vel);
    }
    for (i = 0; i < 8; ++i) {
        angle = TSAR_TAU * tsar_rand_unit();
        speed = 4.0f + 8.0f * tsar_rand_unit();
        vel[0] = cosf(angle) * speed; vel[1] = 6.0f + 8.0f * tsar_rand_unit(); vel[2] = sinf(angle) * speed;
        emit_tsar(TSAR_FIRE, p, vel);
    }
    for (i = 0; i < 24; ++i) {
        angle = TSAR_TAU * (float)i / 24.0f;
        p[0] = s->pos[0] + cosf(angle) * 12.0f;
        p[1] = s->pos[1] + 2.0f;
        p[2] = s->pos[2] + sinf(angle) * 12.0f;
        vel[0] = cosf(angle) * 60.0f; vel[1] = 2.0f; vel[2] = sinf(angle) * 60.0f;
        emit_tsar(TSAR_DUST, p, vel);
    }
    for (i = 0; i < 8; ++i) {
        angle = TSAR_TAU * tsar_rand_unit();
        p[0] = s->pos[0]; p[1] = s->pos[1] + 20.0f; p[2] = s->pos[2];
        vel[0] = cosf(angle) * 15.0f; vel[1] = 25.0f; vel[2] = sinf(angle) * 15.0f;
        emit_tsar(TSAR_SMOKE, p, vel);
    }
}

static void tsar_step(BlastSlot *s)
{
    unsigned j;
    float p[3], vel[3], angle, radius, height, unfurl;
    float cap_h = (s->tsar == 2u) ? 90.0f : 75.0f;
    float cap_r = (s->tsar == 2u) ? 75.0f : 60.0f;
    /* the single cap unfurls outward over ~12 seconds */
    unfurl = s->age / 12.0f;
    if (unfurl > 1.0f) unfurl = 1.0f;

    /* orange blast wave: low ring hugging the ground, opening in all
       directions around the city (first ~6s), with a grey smoke layer
       rolling just above it */
    if (s->age < 10.0f) {
        radius = 30.0f + s->age * 140.0f;
        for (j = 0; j < 24; ++j) {
            angle = TSAR_TAU * (float)j / 20.0f + s->age * 2.0f;
            p[0] = s->pos[0] + cosf(angle) * radius;
            p[2] = s->pos[2] + sinf(angle) * radius;
            p[1] = s->pos[1] + 2.5f;
            if (g_fx && g_fx->sample_terrain_height &&
                g_fx->sample_terrain_height(p[0], p[2], &height) && isfinite(height))
                p[1] = height + 2.5f;
            vel[0] = cosf(angle) * 150.0f;
            vel[1] = 1.0f;
            vel[2] = sinf(angle) * 150.0f;
            emit_tsar(TSAR_FIRE, p, vel);
            if (j % 2u == 0u) {
                float q[3];
                q[0] = p[0]; q[2] = p[2];
                q[1] = p[1] + 10.0f;
                vel[0] = cosf(angle) * 120.0f;
                vel[1] = 1.0f;
                vel[2] = sinf(angle) * 120.0f;
                emit_tsar(TSAR_WAVE, q, vel);
            }
        }
        /* wave demolition: buildings crumble as the wavefront sweeps past */
        if (g_settings[SET_WAVE_DEMO] && s->age > 0.5f) {
            for (j = 0; j < 2; ++j) {
                float da = TSAR_TAU * tsar_rand_unit();
                float dq[3];
                dq[0] = s->pos[0] + cosf(da) * radius;
                dq[1] = s->pos[1];
                dq[2] = s->pos[2] + sinf(da) * radius;
                queue_meltdown(dq, 70.0f);
            }
        }
        /* sustained bright light at the fireball while the wave expands */
        if (s->age < 2.0f) {
            p[0] = s->pos[0]; p[2] = s->pos[2];
            p[1] = s->pos[1] + cap_h * 0.5f;
            vel[0] = vel[1] = vel[2] = 0.0f;
            emit_tsar(TSAR_FLASH, p, vel);
        }
    }

    /* ONE mushroom cap: orange flame on top, grey smoke underneath
       (Merc-2 look) - rises with the column, then spreads */
    {
        float deck = cap_h * ((s->age < 6.0f) ? (s->age / 6.0f) : 1.0f)
                   + sinf(s->age * 0.6f) * 7.0f;
        for (j = 0; j < 18; ++j) {
            angle = TSAR_TAU * tsar_rand_unit();
            radius = cap_r * sqrtf(unfurl) * sqrtf(tsar_rand_unit());
            p[0] = s->pos[0] + cosf(angle) * radius;
            p[2] = s->pos[2] + sinf(angle) * radius;
            if (j < 9) {
                /* flame: top of the head and cracks burning through it */
                p[1] = s->pos[1] + deck - 10.0f + 16.0f * tsar_rand_unit();
                vel[1] = (s->age < 6.0f) ? 9.0f : 1.5f;
                emit_tsar(TSAR_FIRE, p, vel);
            } else if (j < 14) {
                /* grey smoke under and around the head */
                p[1] = s->pos[1] + deck - 8.0f + 5.0f * tsar_rand_unit();
                vel[1] = (s->age < 6.0f) ? 9.0f : 1.0f;
                emit_tsar(TSAR_SMOKE, p, vel);
            } else {
                /* wider dark skirt under the head (photo look) */
                radius = cap_r * 1.25f * sqrtf(unfurl) * sqrtf(tsar_rand_unit());
                p[0] = s->pos[0] + cosf(angle) * radius;
                p[2] = s->pos[2] + sinf(angle) * radius;
                p[1] = s->pos[1] + deck - 14.0f + 4.0f * tsar_rand_unit();
                vel[1] = (s->age < 6.0f) ? 9.0f : 0.8f;
                emit_tsar(TSAR_SMOKE, p, vel);
            }
            vel[0] = cosf(angle) * 3.0f;
            vel[2] = sinf(angle) * 3.0f;
        }
    }
    /* Merc-2 style glow: orange embers under the cap and along the stem */
    if (s->age < 30.0f) {
        for (j = 0; j < 2; ++j) {
            angle = TSAR_TAU * tsar_rand_unit();
            radius = cap_r * 0.6f * tsar_rand_unit();
            p[0] = s->pos[0] + cosf(angle) * radius;
            p[1] = s->pos[1] + cap_h - 8.0f + 4.0f * tsar_rand_unit();
            p[2] = s->pos[2] + sinf(angle) * radius;
            vel[0] = cosf(angle) * 1.0f; vel[1] = 2.0f; vel[2] = sinf(angle) * 1.0f;
            emit_tsar(TSAR_FIRE, p, vel);
        }
        for (j = 0; j < 2; ++j) {
            angle = TSAR_TAU * tsar_rand_unit();
            p[0] = s->pos[0] + cosf(angle) * 2.0f;
            p[1] = s->pos[1] + (cap_h * 0.9f) * tsar_rand_unit();
            p[2] = s->pos[2] + sinf(angle) * 2.0f;
            vel[0] = 0.0f; vel[1] = 5.0f; vel[2] = 0.0f;
            emit_tsar(TSAR_FIRE, p, vel);
        }
    }
    /* narrow solid stem column feeding the deck */
    if (s->age < 30.0f) {
        for (j = 0; j < 3; ++j) {
            angle = TSAR_TAU * tsar_rand_unit();
            radius = 4.0f + 8.0f * tsar_rand_unit();
            p[0] = s->pos[0] + cosf(angle) * radius;
            p[1] = s->pos[1] + 3.0f + (cap_h * 0.9f) * tsar_rand_unit();
            p[2] = s->pos[2] + sinf(angle) * radius;
            vel[0] = cosf(angle) * 0.5f;
            vel[1] = 6.0f + 4.0f * tsar_rand_unit();
            vel[2] = sinf(angle) * 0.5f;
            emit_tsar(TSAR_PLUME, p, vel);
        }
        /* orange fire burning inside the stem (video: glowing cracks) */
        for (j = 0; j < 2; ++j) {
            angle = TSAR_TAU * tsar_rand_unit();
            p[0] = s->pos[0] + cosf(angle) * 3.0f;
            p[1] = s->pos[1] + 3.0f + (cap_h * 0.8f) * tsar_rand_unit();
            p[2] = s->pos[2] + sinf(angle) * 3.0f;
            vel[0] = 0.0f; vel[1] = 5.0f; vel[2] = 0.0f;
            emit_tsar(TSAR_FIRE, p, vel);
        }
    }
}

static void register_tsar_particles(void)
{
    /* Descriptors follow Svered's MOAB authoring (proven in-game), scaled
       up for nuke tier. Textures are resident retail sprites. No flat
       green star overlays: the radiation haze layer was removed. */
    static const uint32_t tex[TSAR_PARTICLES] = {
        0x88e27c45u, /* flash */
        0x02bb1fe0u, /* explosion */
        0xc272d7afu, /* dust */
        0xc272d7afu, /* crown */
        0xc272d7afu, /* plume stem */
        0xc272d7afu, /* ground blanket */
        0xc272d7afu  /* grey sweeping wave */
    };
    static const uint32_t col[TSAR_PARTICLES][3] = {
        { 0x80fff4ccu, 0x60ffbd64u, 0x00ff8000u },
        { 0x80ffffe0u, 0x80ffd040u, 0x30804010u },
        { 0x48bca17cu, 0x30a48c70u, 0x008d806cu },
        { 0x805c564eu, 0x80403c38u, 0x00282420u },
        { 0x806a6258u, 0x80504a44u, 0x00383430u },
        { 0x806a655eu, 0x805a5650u, 0x00484440u }
    };
    static const float wid[TSAR_PARTICLES][3] = {
        { 24.0f, 90.0f, 100.0f }, { 30.0f, 80.0f, 100.0f }, { 18.0f, 55.0f, 90.0f },
        { 40.0f, 75.0f, 100.0f }, { 20.0f, 45.0f, 70.0f }, { 32.0f, 48.0f, 56.0f },
        { 25.0f, 55.0f, 85.0f }
    };
    static const float hei[TSAR_PARTICLES][3] = {
        { 24.0f, 90.0f, 100.0f }, { 30.0f, 80.0f, 100.0f }, { 12.0f, 35.0f, 60.0f },
        { 35.0f, 70.0f, 100.0f }, { 20.0f, 45.0f, 70.0f }, { 12.0f, 14.0f, 16.0f },
        { 12.0f, 25.0f, 40.0f }
    };
    static const float life[TSAR_PARTICLES] = { 2.5f, 2.0f, 8.0f, 16.0f, 14.0f, 16.0f, 2.5f };
    unsigned i;
    g_tsar_ready = 0;
    g_tsar_rand = 0x74736172u;
    /* ini-only option: ViewRange = how far the nuke cloud renders (128-4096) */
    if (g_host) {
        unsigned vr = g_host->read_setting(SETTINGS_FILE, SECTION, "ViewRange", 4096u);
        if (vr < 128u) vr = 128u;
        if (vr > 4096u) vr = 4096u;
        g_tsar_view = (float)vr;
    }
    LOGF("nuke cloud view range: %.0f", g_tsar_view);
    for (i = 0; i < TSAR_PARTICLES; ++i) {
        MercFxParticle p;
        memset(&p, 0, sizeof(p));
        p.size = sizeof(p);
        p.texture_hash = tex[i];
        p.blend_mode = (i == TSAR_FLASH || i == TSAR_FIRE) ? 1 : 0;
        memcpy(p.colors, col[i], sizeof(p.colors));
        memcpy(p.widths, wid[i], sizeof(p.widths));
        memcpy(p.heights, hei[i], sizeof(p.heights));
        p.time_b = (i == TSAR_GROUND) ? 0.25f : 0.35f;
        p.y_acceleration = (i == TSAR_SMOKE) ? 0.4f : (i == TSAR_PLUME) ? 0.8f : 0.0f;
        p.friction = (i == TSAR_DUST) ? 0.2f : 0.1f;
        p.lifetime = life[i];
        p.lifetime_random = 6.0f;
        p.random_scale = 0.4f;
        p.random_acceleration = 2.0f;
        p.wind = 0.2f;
        p.max_distance = g_tsar_view;
        p.flags = 0;
        g_tsar_particles[i] = g_fx->register_particle(&p);
        if (!g_tsar_particles[i]) return;
    }
    g_tsar_ready = 1;
}

static void __cdecl tw_timeline_tick(float dt, void *userdata)
{
    unsigned i, k;
    (void)userdata;
    for (i = 0; i < BLAST_SLOTS; ++i) {
        BlastSlot *s = &g_slots[i];
        if (!s->active) continue;
        s->age += dt;

        /* TSAR particle cloud: fireball, stem, cap, city dust blanket */
        if (s->tsar && g_tsar_ready) {
            s->tsar_pending += dt;
            if (s->tsar_pending >= 0.125f) {
                s->tsar_pending = 0.0f;
                tsar_step(s);
            }
        }

        /* rolling pressure rings across the blast area */
        s->next_ring -= dt;
        if (s->next_ring <= 0.0f && g_fx && g_fx->emit_shockwave) {
            MercFxShockwave sw;
            s->next_ring = s->wide ? 2.0f : 3.5f;
            sw.size = sizeof(sw);
            sw.flags = MERCFX_SHOCKWAVE_RINGS;
            sw.radius = 200.0f;
            sw.duration = 4.0f;
            g_fx->emit_shockwave(s->pos, &sw);
        }

        /* scorch spiral spreading from the crater */
        s->next_scorch -= dt;
        if (s->next_scorch <= 0.0f && g_fx && g_fx->add_ground_scorch && s->age > 0.5f) {
            float ang, dist, p[3];
            s->next_scorch = s->wide ? 1.0f : 1.8f;
            ang = s->scorch_step * 0.9f;
            dist = 10.0f + s->scorch_step * (s->wide ? 12.0f : 7.0f);
            if (dist > 200.0f) dist = 200.0f;
            p[0] = s->pos[0] + cosf(ang) * dist;
            p[1] = s->pos[1];
            p[2] = s->pos[2] + sinf(ang) * dist;
            g_fx->add_ground_scorch(p, 40.0f);
            ++s->scorch_step;
        }

        /* staged wide mushroom: extra Meltdown visuals queued for world_tick */
        if (s->wide && !s->tsar) {
            static const float ages_w[4] = {1.5f, 4.0f, 7.0f, 10.0f};
            static const float radii_w[4] = {700.0f, 800.0f, 900.0f, 900.0f};
            static const float heights_w[4] = {20.0f, 45.0f, 75.0f, 110.0f};
            while (s->meltdown_stage < 4u && s->age >= ages_w[s->meltdown_stage]) {
                float p[3];
                p[0] = s->pos[0];
                p[1] = s->pos[1] + heights_w[s->meltdown_stage];
                p[2] = s->pos[2];
                queue_meltdown(p, radii_w[s->meltdown_stage]);
                ++s->meltdown_stage;
            }
        } else if (!s->tsar) {
            static const float ages_n[3] = {2.0f, 5.0f, 8.0f};
            static const float radii_n[3] = {500.0f, 600.0f, 700.0f};
            static const float heights_n[3] = {15.0f, 35.0f, 60.0f};
            while (s->meltdown_stage < 3u && s->age >= ages_n[s->meltdown_stage]) {
                float p[3];
                p[0] = s->pos[0];
                p[1] = s->pos[1] + heights_n[s->meltdown_stage];
                p[2] = s->pos[2];
                queue_meltdown(p, radii_n[s->meltdown_stage]);
                ++s->meltdown_stage;
            }
        }

        if (s->age >= s->duration) s->active = 0;
    }
    (void)k;
}

static void __cdecl tw_timeline_reset(void *userdata)
{
    (void)userdata;
    memset(g_slots, 0, sizeof(g_slots));
    g_extra_head = 0;
    g_extra_count = 0;
}

static void fx_bind_pass(int announce)
{
    unsigned i, ok = 0;
    if (!g_fx || !g_fx->bind_template) return;
    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) {
        int want = (int)category_enabled(kTriggers[i].category);
        unsigned char b = g_fx->bind_template(kTriggers[i].name, FX_EFFECT, want) ? 1u : 0u;
        g_template_hash[i] = fnv1a(kTriggers[i].name);
        if (b) ++ok;
        if ((b != g_bound[i] || announce) && g_settings[SET_DEBUG_LOG]) {
            LOGF("bind %s: %s", kTriggers[i].name, b ? "BOUND" : "unavailable/owned");
        }
        g_bound[i] = b;
    }
    g_bound_count = ok;
    if (announce) LOGF("MercFx bind: %u of %u templates bound", g_bound_count, (unsigned)TRIGGER_COUNT);
}

static void try_attach_fx(void)
{
    HMODULE mod;
    MercFxGetApi get_api;
    if (!g_host || g_fx_state == 2 || g_fx_state == 3) return;

    mod = GetModuleHandleW(L"native_effects.dll");
    if (!mod) {
        if (++g_fx_misses >= 600u) {
            g_fx_state = 3;
            LOGF("native_effects.dll not loaded; enhanced blast FX unavailable (use Native Effects)");
        }
        return;
    }
    get_api = (MercFxGetApi)(void *)GetProcAddress(mod, "MercFx_GetApi");
    if (!get_api) { g_fx_state = 3; LOGF("native_effects.dll missing MercFx_GetApi"); return; }
    g_fx = get_api(MERCFX_API_VERSION);
    if (!g_fx || g_fx->version != MERCFX_API_VERSION) {
        if (++g_fx_attempts >= 3u) { g_fx_state = 3; LOGF("MercFx API version mismatch"); }
        return;
    }
    if (g_fx->size < offsetof(MercFxApi, add_ground_scorch) + sizeof(g_fx->add_ground_scorch) ||
        !g_fx->register_effect || !g_fx->bind_template || !g_fx->apply_blast_configured ||
        !g_fx->emit_shockwave || !g_fx->request_slowdown) {
        g_fx_state = 3; LOGF("MercFx API incomplete; enhanced blast FX unavailable"); return;
    }
    if (!g_fx->register_effect(FX_EFFECT, fx_impact, NULL)) {
        if (++g_fx_attempts >= 3u) { g_fx_state = 3; LOGF("MercFx effect registration failed"); }
        return;
    }
    g_fx_state = 2;
    if (g_fx->register_simulation && !g_sim_registered) {
        if (g_fx->register_simulation(tw_timeline_tick, tw_timeline_reset, NULL)) {
            g_sim_registered = 1;
            LOGF("rolling shockwave/mushroom timeline registered");
        }
    }
    g_nuke_sound = 0;
    if (g_fx->size >= offsetof(MercFxApi, play_sound) + sizeof(g_fx->play_sound) &&
        g_fx->register_sound && g_fx->sound_ready && g_fx->play_sound)
        g_nuke_sound = g_fx->register_sound("a_strike.bb_explosion");
    if (g_nuke_sound) LOGF("nuke blast sound registered (a_strike.bb_explosion)");
    if (g_fx->size >= offsetof(MercFxApi, register_particle) + sizeof(g_fx->register_particle) &&
        g_fx->register_particle && g_fx->emit_particle && g_fx->sample_terrain_height) {
        register_tsar_particles();
        LOGF(g_tsar_ready
            ? "TSAR nuke cloud registered: fireball, giant mushroom cap, stem, city dust blanket, earthquake shake, blast sound"
            : "TSAR visuals failed to register (particle budget/texture unavailable)");
    } else {
        LOGF("TSAR visuals unavailable: Native Effects build lacks particle API; meltdown visuals only");
    }
    fx_bind_pass(1);
    {
        unsigned j, st = 0;
        for (j = 0; j < (unsigned)TRIGGER_COUNT; ++j)
            if (kTriggers[j].category == CAT_STRATEGIC && g_bound[j]) st = 1;
        if (!st)
            LOGF("note: strategic missile detonation is engine-scripted; for the TSAR nuke use BUNKER BUSTER (NUKE) or FUEL-AIR (FOAB)");
    }
    LOGF("MercFx attached: enhanced airstrike blasts enabled where templates are bound");
}

static MercFxDecision __cdecl fx_impact(const MercFxImpact *event, void *userdata)
{
    MercFxImpact *ev;
    MercFxBlastOptions opt;
    MercFxShockwave sw;
    unsigned i, cat = 0xFFu;
    float dmg_m = 1.0f, rad_m = 1.0f, force_m = 1.0f;
    int big = 0, do_meltdown = 0;

    (void)userdata;
    if (!g_settings[SET_MASTER] || !event) return MERCFX_UNHANDLED;

    for (i = 0; i < (unsigned)TRIGGER_COUNT; ++i) {
        if (event->template_hash == g_template_hash[i]) { cat = kTriggers[i].category; break; }
    }
    if (cat == 0xFFu) {
        /* unknown template: catch-all for explosive weapon impacts (rockets,
           tank shells, grenades, mortars...) when WEAPON REALISM is ON */
        if (g_settings[SET_WEAPON_REALISM] && event->splash_radius >= 25.0f) {
            ev = (MercFxImpact *)event;
            ev->splash_damage *= 1.6f;
            ev->splash_radius *= 1.4f;
            ev->splash_force *= 1.4f;
            if (ev->splash_radius > 400.0f) ev->splash_radius = 400.0f;
            opt.size = sizeof(opt);
            opt.damage_type = MERCFX_BLAST_FROM_PROJECTILE;
            opt.shake_duration = 0.6f;
            opt.shake_magnitude = 0.6f;
            opt.scorch_radius = 15.0f;
            opt.suppress_default_sound = 0u;
            if (!g_fx->apply_blast_configured(event, &opt)) {
                opt.damage_type = MERCFX_BLAST_STANDARD;
                if (!g_fx->apply_blast_configured(event, &opt)) {
                    if (g_settings[SET_DEBUG_LOG]) LOGF("unknown impact hash=%08X rejected", event->template_hash);
                    return MERCFX_UNHANDLED;
                }
            }
            if (g_settings[SET_DEBUG_LOG]) LOGF("weapon impact hash=%08X r=%.1f enhanced", event->template_hash, event->splash_radius);
            return MERCFX_REMOVE;
        }
        if (g_settings[SET_DEBUG_LOG]) LOGF("impact ignored hash=%08X r=%.1f", event->template_hash, event->splash_radius);
        return MERCFX_UNHANDLED;
    }
    if (!category_enabled(cat)) return MERCFX_UNHANDLED;

    ev = (MercFxImpact *)event;
    switch (cat) {
        case CAT_SMART:
            dmg_m = 2.0f; rad_m = 1.8f; force_m = 1.6f; break;
        case CAT_SURGICAL:
            dmg_m = 2.5f; rad_m = 1.5f; force_m = 1.8f; break;
        case CAT_CARPET:
            dmg_m = 1.8f; rad_m = 2.2f; force_m = 1.8f; break;
        case CAT_BUNKER:
            if (g_settings[SET_BUNKER_NUKE] == 2u) { dmg_m = 10.0f; rad_m = 6.0f; force_m = 7.0f; big = 1; do_meltdown = 1; }
            else { dmg_m = 3.5f; rad_m = 2.5f; force_m = 3.0f; big = 1; }
            break;
        case CAT_CRUISE:
            if (g_settings[SET_CRUISE_FOAB] == 3u) { dmg_m = 10.0f; rad_m = 6.0f; force_m = 7.0f; big = 1; do_meltdown = 1; }
            else if (g_settings[SET_CRUISE_FOAB] == 2u) { dmg_m = 6.0f; rad_m = 4.0f; force_m = 5.0f; big = 1; }
            else { dmg_m = 3.0f; rad_m = 2.2f; force_m = 2.5f; big = 1; }
            break;
        case CAT_STRATEGIC: {
            /* 0 OFF, 1 ON, 2 TSAR (60s mushroom), 3 MEGA (120s + city blanket) */
            unsigned lvl = g_settings[SET_STRATEGIC_NUKE];
            if (lvl < 1u) return MERCFX_UNHANDLED;
            dmg_m = (lvl == 1u) ? 10.0f : (lvl == 2u) ? 11.0f : 12.0f;
            rad_m = (lvl == 1u) ? 6.0f : (lvl == 2u) ? 6.5f : 7.0f;
            force_m = (lvl == 1u) ? 7.0f : (lvl == 2u) ? 7.5f : 8.0f;
            big = 1; do_meltdown = 1; break;
        }
        case CAT_C4:
            /* Best-effort blast enhancement on C4 impact events (detonator range is engine-side). */
            if (g_settings[SET_C4_MEGA] == 3u) { dmg_m = 6.0f; rad_m = 5.0f; force_m = 4.5f; big = 1; }
            else if (g_settings[SET_C4_MEGA] == 2u) { dmg_m = 4.5f; rad_m = 3.5f; force_m = 3.5f; big = 1; }
            else { dmg_m = 3.0f; rad_m = 2.0f; force_m = 2.5f; }
            break;
        case CAT_WEAPON:
            /* Best-effort enhancement for explosive weapon impacts (rockets/shells/grenades/mortars). */
            dmg_m = 1.8f; rad_m = 1.6f; force_m = 1.5f; break;
        default:
            return MERCFX_UNHANDLED;
    }

    ev->splash_damage *= dmg_m;
    ev->splash_radius *= rad_m;
    ev->splash_force *= force_m;
    /* nuke tier: city-destroying blast - at least 1K radius (WiC city nuke) */
    if (do_meltdown && ev->splash_radius < 1000.0f) ev->splash_radius = 1000.0f;
    if (ev->splash_radius > 1000.0f) ev->splash_radius = 1000.0f;

    {
        int nuke_audio = 0;
        if (do_meltdown && g_nuke_sound && g_fx->sound_ready)
            nuke_audio = g_fx->sound_ready(g_nuke_sound);
        opt.size = sizeof(opt);
        /* nuke tier: plain explosion visual (meltdown look, no red streaks) */
        opt.damage_type = do_meltdown ? MERCFX_BLAST_STANDARD
                        : (big ? MERCFX_BLAST_BUNKERBUSTER : MERCFX_BLAST_FROM_PROJECTILE);
        /* nuke tier: earthquake per setting; others: standard */
        if (do_meltdown) {
            switch (g_settings[SET_EARTHQUAKE]) {
                case 2u:  opt.shake_duration = 20.0f; opt.shake_magnitude = 5.0f; break;
                case 1u:  opt.shake_duration = 10.0f; opt.shake_magnitude = 3.0f; break;
                default:  opt.shake_duration = 1.0f;  opt.shake_magnitude = 1.0f; break;
            }
        } else {
            opt.shake_duration = big ? 3.0f : 0.8f;
            opt.shake_magnitude = big ? 3.0f : 0.8f;
        }
        opt.scorch_radius = big ? 40.0f : 20.0f;
        /* NUKE ONLY silences the built-in blast so the cue plays alone */
        opt.suppress_default_sound = (nuke_audio && g_settings[SET_NUKE_AUDIO] == 2u) ? 1u : 0u;
        if (nuke_audio && g_settings[SET_NUKE_AUDIO] != 0u) {
            ev = (MercFxImpact *)event; /* remember to play after the blast */
            g_nuke_audio_pending = 1;
        }
    }

    if (!g_fx->apply_blast_configured(event, &opt)) {
        if (!big) {
            /* unsupported raw damage types reject FROM_PROJECTILE; retry as STANDARD */
            opt.damage_type = MERCFX_BLAST_STANDARD;
            if (!g_fx->apply_blast_configured(event, &opt)) {
                g_pending_log_fail = 1;
                return MERCFX_UNHANDLED;
            }
        } else {
            g_pending_log_fail = 1;
            return MERCFX_UNHANDLED;
        }
    }

    if (g_nuke_audio_pending) {
        g_nuke_audio_pending = 0;
        g_fx->play_sound(g_nuke_sound, event->position);
        LOGF("nuke blast sound played (a_strike.bb_explosion)");
    }

    if (big) {
        sw.size = sizeof(sw);
        sw.flags = MERCFX_SHOCKWAVE_REFRACTION | MERCFX_SHOCKWAVE_RINGS;
        sw.radius = (rad_m >= 5.0f) ? 260.0f : 120.0f;
        sw.duration = (rad_m >= 5.0f) ? 6.0f : 2.0f;
        g_fx->emit_shockwave(event->position, &sw);
        if (rad_m >= 5.0f) {
            /* second, wider pressure wave for nuke-tier blasts */
            MercFxShockwave sw2;
            memset(&sw2, 0, sizeof(sw2));
            sw2.size = sizeof(sw2);
            sw2.flags = MERCFX_SHOCKWAVE_RINGS;
            sw2.radius = 400.0f;
            sw2.duration = 8.0f;
            g_fx->emit_shockwave(event->position, &sw2);
            g_fx->request_slowdown(FX_OWNER, 0.30f, 3.0f);
        } else {
            g_fx->request_slowdown(FX_OWNER, 0.60f, 0.8f);
        }
    }

    if (do_meltdown) {
        memcpy(g_pending_pos, event->position, sizeof(g_pending_pos));
        /* MELTDOWN look: the retail meltdown explosion is the primary blast */
        g_pending_meltdown_r = (g_settings[SET_NUKE_EFFECT] == 1u) ? 1100.0f : 500.0f;
        g_pending_meltdown = 1;
        /* Merc-2-style cinematic flash: two extra wide fireballs on the same
           frame, then the staged mushroom timeline keeps the cloud growing */
        queue_meltdown(event->position, 1000.0f);
        queue_meltdown(event->position, 750.0f);
    }

    if (big) {
        BlastSlot *slot = NULL;
        unsigned k;
        for (k = 0; k < BLAST_SLOTS; ++k) {
            BlastSlot *c = &g_slots[g_slot_cursor];
            g_slot_cursor = (g_slot_cursor + 1u) % BLAST_SLOTS;
            if (!c->active) { slot = c; break; }
        }
        if (!slot) slot = &g_slots[0];
        memset(slot, 0, sizeof(*slot));
        slot->active = 1;
        slot->wide = (rad_m >= 5.0f);
        {
            unsigned sv = g_settings[SET_STRATEGIC_NUKE];
            slot->tsar = (do_meltdown && slot->wide && sv >= 2u) ? ((sv == 2u) ? 1u : 2u) : 0u;
        }
        slot->tsar_sample = 0;
        slot->tsar_pending = 0.0f;
        memcpy(slot->pos, event->position, sizeof(slot->pos));
        if (slot->tsar) slot->duration = (slot->tsar == 2u) ? 120.0f : 60.0f;
        else slot->duration = slot->wide ? 30.0f : 12.0f;
        if (slot->tsar && g_tsar_ready) tsar_burst(slot);
        slot->next_ring = 2.5f;
        slot->next_scorch = 2.0f;
    }

    _snprintf(g_pending_log_tpl, sizeof(g_pending_log_tpl) - 1, "%s", kTriggers[i].name);
    g_pending_log_tpl[sizeof(g_pending_log_tpl) - 1] = 0;
    return MERCFX_REMOVE;
}

static void register_mod_strings(void)
{
    if (!g_host) return;
    if (g_host->size < offsetof(MercenariesModHost, register_string) + sizeof(g_host->register_string)) return;
    if (!g_host->register_string) return;
    g_host->register_string("totalwarfare.name", "TOTAL WARFARE OVERHAUL");
    g_host->register_string("totalwarfare.tagline", "Enhanced ordnance, mobility, and sandbox profiles");
}

static int try_add_shop_item(const char *tpl, const char *name, const char *desc, const char *detail,
                             const char *tex, unsigned price, unsigned category)
{
    MercenariesShopItem item;
    if (!g_host || !g_host->add_shop_item) return 0;
    if (g_host->size < offsetof(MercenariesModHost, add_shop_item) + sizeof(g_host->add_shop_item)) return 0;
    item.template_name = tpl; item.display_name = name; item.description = desc;
    item.detailed_description = detail; item.texture = tex; item.price = price; item.category = category;
    return g_host->add_shop_item(&item);
}

static void attempt_shop_registration(void)
{
    int added = 0;
    if (!g_host || g_shop_attempted) return;
    if (!g_settings[SET_MASTER] || g_settings[SET_SHOP_PROFILE] == 0u) { g_shop_attempted = 1u; return; }
    if (g_settings[SET_BUNKER_NUKE] == 2u)
        added += try_add_shop_item("template_support_p_bunkerbuster_nuke", "totalwarfare.name",
                                   "Nuclear bunker buster profile", "Nuclear bunker buster profile",
                                   "shop_bunkerbuster", 500000u, MERCENARIES_SHOP_AIR_STRIKES);
    if (g_settings[SET_CRUISE_FOAB] >= 2u)
        added += try_add_shop_item("template_support_p_cruise_foab", "totalwarfare.name",
                                   "Thermobaric/FOAB cruise profile", "Thermobaric/FOAB cruise profile",
                                   "shop_cruisemissile", 350000u, MERCENARIES_SHOP_AIR_STRIKES);
    if (g_settings[SET_STRATEGIC_NUKE])
        added += try_add_shop_item("template_support_p_strategic_nuke", "totalwarfare.name",
                                   "Strategic nuclear missile profile", "Strategic nuclear missile profile",
                                   "shop_strategicmissile", 999999u, MERCENARIES_SHOP_AIR_STRIKES);
    g_shop_attempted = 1u;
    if (g_settings[SET_DEBUG_LOG]) LOGF("shop extras added: %d (duplicates/unavailable are normal)", added);
}

static void world_tick(uint32_t delta_bits)
{
    float delta;
    memcpy(&delta, &delta_bits, sizeof(delta));
    if (!(delta > 0.0f) || delta > 10.0f) delta = 0.0f;
    if (!g_host || !g_settings[SET_MASTER]) return;

    g_world_time += delta;
    g_ticks++;

    if (g_fx_state != 2 && g_fx_state != 3) try_attach_fx();

    if (g_fx_state == 2 && g_bound_count < (unsigned)TRIGGER_COUNT) {
        if (g_next_rebind == 0.0) g_next_rebind = g_world_time + 2.0;
        else if (g_world_time >= g_next_rebind) { g_next_rebind = g_world_time + 2.0; fx_bind_pass(0); }
    }

    if (!g_shop_attempted) { register_mod_strings(); attempt_shop_registration(); }

    if (g_pending_log_fail) { g_pending_log_fail = 0; LOGF("MercFx blast rejected; vanilla explosion used"); }
    if (g_pending_log_tpl[0]) { LOGF("enhanced blast on %s", g_pending_log_tpl); g_pending_log_tpl[0] = 0; }

    if (g_pending_meltdown) {
        g_pending_meltdown = 0;
        if (!g_checked_retail) {
            g_have_explosion_fn = g_host->guest_function_exists(EXPLOSION_FN) ? 1 : 0;
            g_meltdown_hash = fnv1a("Meltdown");
            g_checked_retail = 1;
        }
        if (g_have_explosion_fn) {
            uint32_t xp[13];
            memcpy(xp, g_pending_pos, 12);
            xp[3] = fbits(g_pending_meltdown_r); xp[4] = fbits(0.0f); xp[5] = 0; xp[6] = 0;
            xp[7] = g_meltdown_hash; xp[8] = 0; xp[9] = 0; xp[10] = 0; xp[11] = 0; xp[12] = 0;
            g_host->guest_call(EXPLOSION_FN, 0, 13, xp);
            LOGF("Meltdown nuke effect fired r=%.0f", g_pending_meltdown_r);
        }
    }

    /* staged wide/narrow mushroom extras queued by the blast timeline */
    while (g_extra_count) {
        float p[3], r;
        unsigned idx = g_extra_head;
        memcpy(p, g_extra_pos[idx], sizeof(p));
        r = g_extra_radius[idx];
        g_extra_head = (g_extra_head + 1u) % EXTRA_QUEUE;
        --g_extra_count;
        if (g_have_explosion_fn) {
            uint32_t xp[13];
            memcpy(xp, p, 12);
            xp[3] = fbits(r); xp[4] = fbits(0.0f); xp[5] = 0; xp[6] = 0;
            xp[7] = g_meltdown_hash; xp[8] = 0; xp[9] = 0; xp[10] = 0; xp[11] = 0; xp[12] = 0;
            g_host->guest_call(EXPLOSION_FN, 0, 13, xp);
            if (r < 200.0f) LOGF("wave demolition strike r=%.0f", r);
            else LOGF("staged mushroom visual r=%.0f", r);
        }
    }

    if (!g_announced && g_ticks > 300u) {
        g_announced = 1u;
        LOGF("Total Warfare Overhaul %s active", MOD_VERSION);
        if (g_fx_state == 2) LOGF("Native Effects (MercFx): attached, %u/%u templates bound", g_bound_count, (unsigned)TRIGGER_COUNT);
        else if (g_fx_state == 3) LOGF("Native Effects (MercFx): unavailable; load Native Effects for enhanced blasts/FX");
        if (g_settings[SET_C4_MEGA] || g_settings[SET_WEAPON_REALISM])
            LOGF("note: C4 detonator range and weapon physics need runtime or game-data mods");
    }

    if (g_settings[SET_DEBUG_LOG] && (g_ticks % 3600u) == 0u) LOGF("heartbeat ticks=%u fx=%d", g_ticks, g_fx_state);
}

static int key_pressed(unsigned virtual_key) { (void)virtual_key; return 0; }

static void shutdown_hook(void)
{
    if (g_fx && g_fx->cancel_slowdown) g_fx->cancel_slowdown(FX_OWNER);
}

static const MercenariesMod descriptor = {
    MERCENARIES_MOD_API_VERSION,
    sizeof(MercenariesMod),
    MOD_NAME,
    MOD_VERSION,
    settings,
    SETTING_COUNT,
    world_tick,
    NULL,
    NULL,
    key_pressed,
    shutdown_hook,
    MOD_CREDITS,
    "Airstrike/ordnance/explosive overhaul with optional Native Effects (MercFx) enhanced blasts, shockwaves and nuke visuals. Detonator range, ballistics, mobility, vehicles and map freedom need runtime/data mods.",
    setting_descriptions,
    setting_controls,
    NULL,
    NULL,
    setting_defaults,
};

__declspec(dllexport) const MercenariesMod *mercenaries_mod_entry(const MercenariesModHost *table)
{
    if (!table || table->api_version != MERCENARIES_MOD_API_VERSION || table->size < sizeof(*table)) return NULL;
    g_host = table;
    g_ticks = 0u; g_announced = 0u; g_shop_attempted = 0u;
    g_fx = NULL; g_fx_state = 0; g_fx_misses = 0; g_fx_attempts = 0;
    g_bound_count = 0; g_world_time = 0.0; g_next_rebind = 0.0;
    g_pending_meltdown = 0; g_checked_retail = 0; g_have_explosion_fn = 0;
    g_pending_log_fail = 0; g_pending_log_tpl[0] = 0;
    memset(g_bound, 0, sizeof(g_bound));
    memset(g_template_hash, 0, sizeof(g_template_hash));
    load_settings();
    LOGF("loaded %s %s (Native Effects integration: best-effort)", MOD_NAME, MOD_VERSION);
    try_attach_fx();
    return &descriptor;
}
