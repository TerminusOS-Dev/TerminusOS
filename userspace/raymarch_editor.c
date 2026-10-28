/*
 * raymarch_640_editor.c
 * Terminus OS — Empty scene + Object Editor
 *
 * Internal render: 320x240
 * Output:         640x480 (2x nearest-neighbor)
 *
 * Controls (Edit mode - press Tab):
 *   1          - Spawn Sphere
 *   2          - Spawn Cube
 *   Del/Bksp   - Delete selected
 *   Left/Right - Select prev/next object
 *   R/G/B      - Cycle color (Red/Green/Blue families)
 *   M          - Cycle motion: Static -> Bounce -> Circle
 *   T          - Cycle material: Plastic -> Iron -> Jelly -> Mirror
 *   +/-        - Change size
 *   Q/E        - Rotate cube (yaw)
 *   WASD       - Move selected object
 *   Mouse      - Look (always)
 *
 * Always:
 *   Tab        - Toggle Edit mode
 *   K          - Toggle Physics (gravity + collisions + player capsule)
 *   Space      - Pause animation
 *   F          - Quality toggle
 *   X/Esc      - Exit
 *
 * Player: invisible vertical capsule (elongated sphere) at camera position.
 * Objects bounce off the player when physics is enabled.
 */

#include "terminus_gfx.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#define OUT_W 640
#define OUT_H 480

#define LW 320
#define LH 240

#define EPS             0.0025f
#define MIN_STEP        0.0015f
#define MAX_RAY_DIST    40.0f
#define MAX_OBJ_DIST    40.0f
#define MAX_OBJ         16

typedef struct {
    float x, y, z;
} vec3;

static inline vec3 v3(float x, float y, float z)
{
    vec3 r = { x, y, z };
    return r;
}

static inline vec3 vadd(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 vmul(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vlen2(vec3 a) { return vdot(a, a); }

static inline vec3 vnorm(vec3 a)
{
    float l2 = vlen2(a);
    if (l2 < 1e-12f) return v3(0.0f, 1.0f, 0.0f);
    return vmul(a, 1.0f / sqrtf(l2));
}

static inline vec3 vcross(vec3 a, vec3 b)
{
    return v3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

static inline float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

/* Terminus 6x6x6 palette */
static inline uint8_t cube(int r, int g, int b)
{
    if (r < 0) r = 0; if (r > 5) r = 5;
    if (g < 0) g = 0; if (g > 5) g = 5;
    if (b < 0) b = 0; if (b > 5) b = 5;
    return (uint8_t)(32 + r * 36 + g * 6 + b);
}

static const uint8_t dither8[8][8] = {
    {  3, 38,  9, 40, 25, 27, 15, 34 },
    { 46, 33, 20, 39, 43,  8, 44, 29 },
    { 19, 28, 41, 13, 61, 55,  6, 17 },
    { 54, 51, 49, 21, 42, 63, 31, 26 },
    {  5, 23,  7, 60, 30, 22, 48, 50 },
    { 24, 36, 37, 52, 35, 53, 57,  1 },
    {  2, 14, 12, 45, 47, 16, 58, 56 },
    { 62, 10, 59, 11, 32,  4,  0, 18 }
};

static inline float color_curve(float x)
{
    x = clampf(x, 0.0f, 1.0f);
    if (x > 0.70f) {
        float q = (x - 0.70f) / 0.30f;
        q = q * (0.85f + 0.15f * q);
        x = 0.70f + q * 0.30f;
    }
    return clampf(x, 0.0f, 1.0f);
}

static inline uint8_t rgb_pal(float r, float g, float b, int x, int y)
{
    r = color_curve(r);
    g = color_curve(g);
    b = color_curve(b);

    float dr = ((float)dither8[y & 7][x & 7] - 31.5f) * 0.00050f;
    float dg = ((float)dither8[(y + 5) & 7][(x + 3) & 7] - 31.5f) * 0.00050f;
    float db = ((float)dither8[(y + 2) & 7][(x + 5) & 7] - 31.5f) * 0.00050f;

    r = clampf(r + dr, 0.0f, 1.0f);
    g = clampf(g + dg, 0.0f, 1.0f);
    b = clampf(b + db, 0.0f, 1.0f);

    return cube(
        (int)(r * 5.0f + 0.5f),
        (int)(g * 5.0f + 0.5f),
        (int)(b * 5.0f + 0.5f)
    );
}

/* ------------------------------------------------------------------------- */
/* Object definition                                                         */
/* ------------------------------------------------------------------------- */

/* Material types */
#define MAT_PLASTIC  0
#define MAT_IRON     1
#define MAT_JELLY    2
#define MAT_MIRROR   3
#define MAT_COUNT    4

typedef struct {
    vec3   base;        /* base position (center of motion / spawn point) */
    vec3   pos;         /* current position (updated each frame) */
    vec3   vel;         /* velocity (used when physics is on) */
    float  br;          /* bounding radius for culling */
    float  size;        /* radius (sphere) or half-extent (box) */
    int    type;        /* 1 = sphere, 2 = box */
    int    color_id;    /* 0..7 color preset */
    int    mat_type;    /* MAT_PLASTIC / IRON / JELLY / MIRROR */
    int    motion;      /* 0=static, 1=bounce, 2=circle (ignored when physics on) */
    float  phase;       /* motion phase offset */
    float  yaw;         /* rotation for boxes */
    float  amp;         /* amplitude of motion */
    float  speed;       /* motion speed multiplier */
} Obj;

static Obj objs[MAX_OBJ];
static int nobj = 0;
static int selected = -1;   /* -1 = none */

static int active[MAX_OBJ];
static int nactive = 0;

static float time_f = 0.0f;
static int   edit_mode = 0;
static int   paused = 0;
static int   hq = 0;
static int   physics_on = 0;

#define GRAVITY     18.0f
#define RESTITUTION 0.55f
#define FRICTION    0.987f   /* per-frame velocity keep (was 0.82 — too sticky) */
#define DT          0.045f   /* matches time step */

/* player movement velocity (for transferring kick force to objects) */
static vec3 player_vel;

/* Player capsule (invisible, only for collisions)
 * Vertical capsule centered around camera position.
 * Looks like an elongated sphere / capsule. */
#define PLAYER_RADIUS   0.42f
#define PLAYER_HALF_H   0.95f   /* half-height of the inner segment */

/* Color presets (vibrant) */
static const vec3 color_presets[8] = {
    { 0.95f, 0.12f, 0.12f }, /* 0 Red */
    { 0.12f, 0.85f, 0.22f }, /* 1 Green */
    { 0.15f, 0.45f, 0.98f }, /* 2 Blue */
    { 0.95f, 0.75f, 0.10f }, /* 3 Yellow */
    { 0.95f, 0.25f, 0.85f }, /* 4 Magenta */
    { 0.10f, 0.85f, 0.90f }, /* 5 Cyan */
    { 0.92f, 0.92f, 0.95f }, /* 6 White/Metal */
    { 0.55f, 0.35f, 0.15f }  /* 7 Brown */
};

static const char *color_names[8] = {
    "Red", "Green", "Blue", "Yellow", "Magenta", "Cyan", "Metal", "Brown"
};

static const char *motion_names[3] = {
    "Static", "Bounce", "Circle"
};

static const char *mat_names[MAT_COUNT] = {
    "Plastic", "Iron", "Jelly", "Mirror"
};

/* ------------------------------------------------------------------------- */
/* Scene / Camera                                                            */
/* ------------------------------------------------------------------------- */

static vec3 light_dir;
static vec3 cam_pos;
static float cam_yaw;
static float cam_pitch;
static vec3 cam_f, cam_r, cam_u;

static float us[LW];
static float vs[LH];
static vec3 row_dir[LH];
static vec3 col_dir[LW];
static uint8_t lowbuf[LW * LH];

static void init_screen_tables(void)
{
    for (int x = 0; x < LW; x++)
        us[x] = 2.0f * ((float)x + 0.5f) / (float)LW - 1.0f;
    for (int y = 0; y < LH; y++)
        vs[y] = 1.0f - 2.0f * ((float)y + 0.5f) / (float)LH;
}

static void reset_cam(void)
{
    cam_pos = v3(0.0f, 2.4f, 8.0f);
    cam_yaw = 0.0f;
    cam_pitch = -0.25f;
}

static void update_cam_basis(void)
{
    cam_pitch = clampf(cam_pitch, -1.55f, 1.55f);

    float cp = cosf(cam_pitch);
    float sp = sinf(cam_pitch);
    float cy = cosf(cam_yaw);
    float sy = sinf(cam_yaw);

    cam_f = vnorm(v3(sy * cp, sp, -cy * cp));
    cam_r = vnorm(vcross(cam_f, v3(0.0f, 1.0f, 0.0f)));
    cam_u = vcross(cam_r, cam_f);
}

/* ------------------------------------------------------------------------- */
/* Object management                                                         */
/* ------------------------------------------------------------------------- */

/* Closest point on the player capsule segment (vertical) to a point p */
static vec3 player_capsule_closest(vec3 p)
{
    /* Capsule segment goes from cam_pos.y - PLAYER_HALF_H to cam_pos.y + PLAYER_HALF_H */
    float y = clampf(p.y, cam_pos.y - PLAYER_HALF_H, cam_pos.y + PLAYER_HALF_H);
    return v3(cam_pos.x, y, cam_pos.z);
}

static void collide_objects_with_player(void)
{
    for (int i = 0; i < nobj; i++) {
        Obj *o = &objs[i];

        vec3 closest = player_capsule_closest(o->pos);
        vec3 d = vsub(o->pos, closest);
        float dist2 = vlen2(d);
        float min_dist = o->size + PLAYER_RADIUS;
        float min_dist2 = min_dist * min_dist;

        if (dist2 < min_dist2 && dist2 > 1e-10f) {
            float dist = sqrtf(dist2);
            float overlap = min_dist - dist;
            vec3 n = vmul(d, 1.0f / dist);

            /* push object out (player is kinematic) */
            o->pos = vadd(o->pos, vmul(n, overlap + 0.01f));

            /* relative velocity vs player */
            vec3 vrel = vsub(o->vel, player_vel);
            float vn = vdot(vrel, n);

            float rest = RESTITUTION;
            if (o->mat_type == MAT_JELLY) rest = 0.82f;
            else if (o->mat_type == MAT_IRON) rest = 0.30f;
            else if (o->mat_type == MAT_MIRROR) rest = 0.50f;

            if (vn < 0.0f) {
                /* reflect normal component of relative velocity */
                o->vel = vsub(o->vel, vmul(n, vn * (1.0f + rest)));
            }

            /* transfer some of player's motion into the object (the "kick") */
            float pvn = vdot(player_vel, n);
            if (pvn > 0.0f) {
                /* player is moving into the object — add push */
                float push = pvn * 1.15f;
                o->vel = vadd(o->vel, vmul(n, push));
            }

            /* small tangential friction so it doesn't stick to the player */
            vec3 vtan = vsub(o->vel, vmul(n, vdot(o->vel, n)));
            o->vel = vadd(vmul(n, vdot(o->vel, n)), vmul(vtan, 0.92f));

        } else if (dist2 <= 1e-10f) {
            o->pos.x += min_dist;
            o->vel.x += player_vel.x + 1.5f;
            o->vel.z += player_vel.z;
        }
    }
}

static void update_object_positions(void)
{
    if (physics_on) {
        /* --- Physics mode --- */
        for (int i = 0; i < nobj; i++) {
            Obj *o = &objs[i];

            /* gravity */
            o->vel.y -= GRAVITY * DT;

            /* integrate */
            o->pos.x += o->vel.x * DT;
            o->pos.y += o->vel.y * DT;
            o->pos.z += o->vel.z * DT;

            /* per-material bounce & friction (friction = velocity kept each frame) */
            float rest = RESTITUTION;
            float fric = FRICTION;          /* ~0.987 — slides for a while */
            if (o->mat_type == MAT_JELLY) {
                rest = 0.80f;
                fric = 0.991f;             /* jelly slides longer */
            } else if (o->mat_type == MAT_IRON) {
                rest = 0.28f;
                fric = 0.975f;             /* iron stops sooner */
            } else if (o->mat_type == MAT_MIRROR) {
                rest = 0.50f;
                fric = 0.985f;
            }

            /* floor collision */
            float floor_y = o->size;
            if (o->pos.y < floor_y) {
                o->pos.y = floor_y;
                if (o->vel.y < 0.0f) {
                    o->vel.y = -o->vel.y * rest;
                    if (o->vel.y < 0.35f)
                        o->vel.y = 0.0f;
                }
                /* ground friction — only while on floor */
                o->vel.x *= fric;
                o->vel.z *= fric;

                /* stop completely only when almost still */
                float hsp2 = o->vel.x * o->vel.x + o->vel.z * o->vel.z;
                if (hsp2 < 0.01f) {
                    o->vel.x = 0.0f;
                    o->vel.z = 0.0f;
                }
            }

            /* keep base in sync so when physics is turned off the object stays */
            o->base = o->pos;
        }

        /* simple inter-object collisions (sphere approximation) */
        for (int i = 0; i < nobj; i++) {
            for (int j = i + 1; j < nobj; j++) {
                Obj *a = &objs[i];
                Obj *b = &objs[j];

                vec3 d = vsub(b->pos, a->pos);
                float dist2 = vlen2(d);
                float min_dist = a->size + b->size;
                float min_dist2 = min_dist * min_dist;

                if (dist2 < min_dist2 && dist2 > 1e-8f) {
                    float dist = sqrtf(dist2);
                    float overlap = min_dist - dist;
                    vec3 n = vmul(d, 1.0f / dist);

                    /* separate */
                    float half = overlap * 0.5f;
                    a->pos = vsub(a->pos, vmul(n, half));
                    b->pos = vadd(b->pos, vmul(n, half));

                    /* relative velocity along normal */
                    vec3 vrel = vsub(b->vel, a->vel);
                    float vn = vdot(vrel, n);

                    if (vn < 0.0f) {
                        float rest = RESTITUTION;
                        if (a->mat_type == MAT_JELLY || b->mat_type == MAT_JELLY)
                            rest = 0.85f;
                        else if (a->mat_type == MAT_IRON && b->mat_type == MAT_IRON)
                            rest = 0.30f;
                        float j_imp = -(1.0f + rest) * vn * 0.5f; /* equal mass */
                        a->vel = vsub(a->vel, vmul(n, j_imp));
                        b->vel = vadd(b->vel, vmul(n, j_imp));
                    }
                }
            }
        }

        /* collide dynamic objects against the invisible player capsule */
        collide_objects_with_player();

        /* re-sync base after player collisions */
        for (int i = 0; i < nobj; i++)
            objs[i].base = objs[i].pos;

    } else {
        /* --- Procedural motion mode --- */
        for (int i = 0; i < nobj; i++) {
            Obj *o = &objs[i];

            o->pos = o->base;
            o->vel = v3(0.0f, 0.0f, 0.0f); /* clear velocity */

            if (o->motion == 1) { /* bounce up-down */
                float t = time_f * o->speed + o->phase;
                o->pos.y = o->base.y + fabsf(sinf(t)) * o->amp;
            }
            else if (o->motion == 2) { /* circle */
                float t = time_f * o->speed + o->phase;
                o->pos.x = o->base.x + cosf(t) * o->amp;
                o->pos.z = o->base.z + sinf(t) * o->amp;
            }
        }
    }

    /* update bounding radii */
    for (int i = 0; i < nobj; i++) {
        Obj *o = &objs[i];
        o->br = o->size * 1.8f + 0.3f;
        if (!physics_on) {
            if (o->motion == 2) o->br += o->amp;
            if (o->motion == 1) o->br += o->amp * 0.5f;
        }
    }
}

static int spawn_object(int type)
{
    if (nobj >= MAX_OBJ)
        return -1;

    Obj *o = &objs[nobj];
    memset(o, 0, sizeof(Obj));

    /* spawn in front of camera */
    float dist = 5.0f;
    o->base = vadd(cam_pos, vmul(cam_f, dist));
    o->base.y = maxf(o->base.y, 0.6f);
    o->pos = o->base;

    o->type = type;
    o->size = (type == 1) ? 0.85f : 0.70f;
    o->color_id = 0;
    o->mat_type = MAT_PLASTIC;
    o->motion = 0;
    o->phase = time_f * 0.7f;
    o->yaw = 0.0f;
    o->amp = 1.2f;
    o->speed = 1.4f;
    o->vel = v3(0.0f, 0.0f, 0.0f);
    o->br = o->size * 1.8f + 0.3f;

    selected = nobj;
    nobj++;
    return selected;
}

static void delete_selected(void)
{
    if (selected < 0 || selected >= nobj)
        return;

    for (int i = selected; i < nobj - 1; i++)
        objs[i] = objs[i + 1];

    nobj--;
    if (nobj == 0)
        selected = -1;
    else if (selected >= nobj)
        selected = nobj - 1;
}

static void cycle_color(int dir)
{
    if (selected < 0) return;
    objs[selected].color_id = (objs[selected].color_id + dir + 8) % 8;
}

static void cycle_motion(void)
{
    if (selected < 0) return;
    objs[selected].motion = (objs[selected].motion + 1) % 3;
}

static void cycle_material(void)
{
    if (selected < 0) return;
    objs[selected].mat_type = (objs[selected].mat_type + 1) % MAT_COUNT;
}

/* ------------------------------------------------------------------------- */
/* SDF & shading                                                             */
/* ------------------------------------------------------------------------- */

static inline float object_sdf(vec3 p, const Obj *o)
{
    vec3 q = vsub(p, o->pos);

    if (o->type == 1) {
        return sqrtf(vlen2(q)) - o->size;
    }

    /* rotated box */
    float ca = cosf(o->yaw);
    float sa = sinf(o->yaw);

    vec3 r = v3(
        ca * q.x + sa * q.z,
        q.y,
       -sa * q.x + ca * q.z
    );

    float dx = fabsf(r.x) - o->size;
    float dy = fabsf(r.y) - o->size;
    float dz = fabsf(r.z) - o->size;

    float ox = maxf(dx, 0.0f);
    float oy = maxf(dy, 0.0f);
    float oz = maxf(dz, 0.0f);

    float outside = sqrtf(ox * ox + oy * oy + oz * oz);
    float inside  = minf(maxf(dx, maxf(dy, dz)), 0.0f);

    return outside + inside;
}

/* Player capsule SDF (vertical elongated sphere) */
static inline float player_sdf(vec3 p)
{
    /* Closest point on vertical segment */
    float y = clampf(p.y, cam_pos.y - PLAYER_HALF_H, cam_pos.y + PLAYER_HALF_H);
    vec3 c = v3(cam_pos.x, y, cam_pos.z);
    return sqrtf(vlen2(vsub(p, c))) - PLAYER_RADIUS;
}

/* include_player: 0 = primary rays (player invisible), 1 = reflection rays (player visible) */
static float map_objects(vec3 p, int include_player)
{
    float d = 1e30f;
    for (int i = 0; i < nactive; i++) {
        float od = object_sdf(p, &objs[active[i]]);
        if (od < d) d = od;
    }
    if (include_player) {
        float pd = player_sdf(p);
        if (pd < d) d = pd;
    }
    return d;
}

/* Returns object index, or -3 for player, or -1 if none */
static int material_objects(vec3 p, int include_player)
{
    float d = 1e30f;
    int idx = -1;
    for (int i = 0; i < nactive; i++) {
        float od = object_sdf(p, &objs[active[i]]);
        if (od < d) {
            d = od;
            idx = active[i];
        }
    }
    if (include_player) {
        float pd = player_sdf(p);
        if (pd < d) {
            d = pd;
            idx = -3; /* special player marker */
        }
    }
    return idx;
}

static vec3 calc_normal(vec3 p, int include_player)
{
    const float e = 0.0028f;
    vec3 k1 = v3( e, -e, -e);
    vec3 k2 = v3(-e, -e,  e);
    vec3 k3 = v3(-e,  e, -e);
    vec3 k4 = v3( e,  e,  e);

    float d1 = map_objects(vadd(p, k1), include_player);
    float d2 = map_objects(vadd(p, k2), include_player);
    float d3 = map_objects(vadd(p, k3), include_player);
    float d4 = map_objects(vadd(p, k4), include_player);

    vec3 n = v3(
         d1 - d2 - d3 + d4,
        -d1 - d2 + d3 + d4,
        -d1 + d2 - d3 + d4
    );
    return vnorm(n);
}

static float shadow_fast(vec3 ro, vec3 rd, int steps)
{
    float t = 0.08f;
    float res = 1.0f;

    for (int i = 0; i < steps; i++) {
        vec3 p = vadd(ro, vmul(rd, t));
        float h = map_objects(p, 0); /* shadows ignore player */
        if (h < 0.0015f) return 0.20f;

        float k = 7.0f * h / t;
        if (k < res) res = k;

        t += clampf(h, 0.06f, 0.75f);
        if (t > 16.0f) break;
    }
    return clampf(res, 0.20f, 1.0f);
}

static vec3 albedo_of(int obj_idx, vec3 p)
{
    if (obj_idx == -3) {
        /* Player capsule appearance in reflections */
        return v3(0.75f, 0.82f, 0.95f); /* soft blue-ish body */
    }
    if (obj_idx < 0) {
        /* floor checker */
        float fx = p.x * 0.65f;
        float fz = p.z * 0.65f;
        int c = ((int)floorf(fx) + (int)floorf(fz)) & 1;
        return c ? v3(0.88f, 0.88f, 0.90f) : v3(0.14f, 0.14f, 0.17f);
    }

    int mt = objs[obj_idx].mat_type;
    int cid = objs[obj_idx].color_id;

    if (mt == MAT_IRON)
        return v3(0.38f, 0.40f, 0.44f); /* dark iron */
    if (mt == MAT_MIRROR)
        return v3(0.92f, 0.93f, 0.95f);
    if (mt == MAT_JELLY) {
        /* brighter / more saturated jelly version of the color */
        vec3 c = color_presets[cid];
        return v3(
            minf(c.x * 1.25f + 0.08f, 1.0f),
            minf(c.y * 1.25f + 0.08f, 1.0f),
            minf(c.z * 1.25f + 0.08f, 1.0f)
        );
    }
    return color_presets[cid];
}

static inline float spec_of(int obj_idx)
{
    if (obj_idx == -3) return 0.45f; /* player */
    if (obj_idx < 0) return 0.06f;

    int mt = objs[obj_idx].mat_type;
    if (mt == MAT_MIRROR) return 1.0f;
    if (mt == MAT_IRON)   return 0.92f;
    if (mt == MAT_JELLY)  return 0.55f;
    /* plastic */
    int cid = objs[obj_idx].color_id;
    if (cid == 6) return 0.70f;
    return 0.28f;
}

static inline float pow64(float x)
{
    x = clampf(x, 0.0f, 1.0f);
    float x2 = x * x;
    float x4 = x2 * x2;
    float x8 = x4 * x4;
    float x16 = x8 * x8;
    float x32 = x16 * x16;
    return x32 * x32;
}

/* Forward */
static vec3 sky(vec3 rd);
static int march(vec3 ro, vec3 rd, float *out_t, int *out_mat, int max_steps, int include_player);

static vec3 shade(vec3 p, vec3 n, vec3 rd, int obj_idx, int shadow_steps, int allow_reflect)
{
    /* Player in reflection */
    if (obj_idx == -3) {
        vec3 alb = albedo_of(-3, p);
        float ndotl = maxf(vdot(n, light_dir), 0.0f);
        vec3 col = vmul(alb, 0.35f + ndotl * 0.65f);
        float fog = minf(vlen2(vsub(p, cam_pos)) * 0.0028f, 0.35f);
        return vadd(vmul(col, 1.0f - fog), vmul(v3(0.60f, 0.76f, 0.94f), fog));
    }

    int mt = (obj_idx >= 0) ? objs[obj_idx].mat_type : MAT_PLASTIC;

    /* ---- Mirror: simple reflection ---- */
    if (mt == MAT_MIRROR && allow_reflect) {
        vec3 V = vmul(rd, -1.0f);
        vec3 refl = vsub(rd, vmul(n, 2.0f * vdot(rd, n)));
        refl = vnorm(refl);

        float t2;
        int mid2;
        vec3 rcol;
        /* Reflection ray CAN see the player */
        if (march(vadd(p, vmul(n, 0.04f)), refl, &t2, &mid2, 14, 1)) {
            vec3 p2 = vadd(p, vmul(refl, t2));
            vec3 n2;
            if (mid2 == -2)
                n2 = v3(0.0f, 1.0f, 0.0f);
            else
                n2 = calc_normal(p2, 1);
            /* no recursive mirrors to keep it fast */
            rcol = shade(p2, n2, refl, mid2, 2, 0);
        } else {
            rcol = sky(refl);
        }

        /* slight Fresnel-ish mix with base */
        float fres = 0.75f + 0.25f * pow64(1.0f - maxf(vdot(n, V), 0.0f));
        vec3 base = v3(0.15f, 0.16f, 0.18f);
        vec3 col = vadd(vmul(base, 1.0f - fres), vmul(rcol, fres));

        float dist2 = vlen2(vsub(p, cam_pos));
        float fog = minf(dist2 * 0.0028f, 0.30f);
        return vadd(vmul(col, 1.0f - fog), vmul(v3(0.60f, 0.76f, 0.94f), fog));
    }

    /* ---- Regular materials ---- */
    vec3 alb = albedo_of(obj_idx, p);

    float ndotl = maxf(vdot(n, light_dir), 0.0f);
    float sh = 1.0f;

    if (ndotl > 0.08f && shadow_steps > 0)
        sh = shadow_fast(vadd(p, vmul(n, 0.045f)), light_dir, shadow_steps);

    sh = mixf(0.28f, 1.0f, sh);

    vec3 V = vmul(rd, -1.0f);
    vec3 H = vnorm(vadd(light_dir, V));

    float ndotv = maxf(vdot(n, V), 0.0f);
    float nh = maxf(vdot(n, H), 0.0f);
    float sp = pow64(nh) * spec_of(obj_idx) * ndotv;

    vec3 sun_col = v3(1.00f, 0.95f, 0.88f);
    vec3 amb_col = v3(0.18f, 0.25f, 0.36f);

    /* Iron: more metallic response */
    if (mt == MAT_IRON) {
        amb_col = v3(0.12f, 0.13f, 0.16f);
        sun_col = v3(0.95f, 0.92f, 0.88f);
        sp *= 1.35f;
    }
    /* Jelly: soft bright highlights + slight translucency feel */
    if (mt == MAT_JELLY) {
        amb_col = v3(0.28f, 0.32f, 0.40f);
        sp *= 0.8f;
        /* fake subsurface: add some light from opposite */
        float back = maxf(vdot(n, vmul(light_dir, -1.0f)), 0.0f);
        alb = vadd(alb, vmul(alb, back * 0.22f));
    }

    vec3 lit = vadd(amb_col, vmul(sun_col, ndotl * sh * 0.88f));
    vec3 col = v3(alb.x * lit.x, alb.y * lit.y, alb.z * lit.z);
    col = vadd(col, vmul(v3(1.0f, 0.98f, 0.90f), sp * sh * 0.85f));

    /* light fog */
    float dist2 = vlen2(vsub(p, cam_pos));
    float fog = minf(dist2 * 0.0028f, 0.38f);
    vec3 fogc = v3(0.60f, 0.76f, 0.94f);

    return vadd(vmul(col, 1.0f - fog), vmul(fogc, fog));
}

static vec3 sky(vec3 rd)
{
    float t = clampf(rd.y * 0.5f + 0.5f, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);

    vec3 horizon = v3(0.60f, 0.76f, 0.94f);
    vec3 zenith  = v3(0.08f, 0.28f, 0.78f);
    vec3 col = vadd(vmul(horizon, 1.0f - t), vmul(zenith, t));

    float s = maxf(vdot(rd, light_dir), 0.0f);
    float g = clampf((s - 0.50f) / 0.50f, 0.0f, 1.0f);
    g = g * g * g;
    float core = clampf((s - 0.92f) / 0.08f, 0.0f, 1.0f);
    core = core * core;

    col = vadd(col, vmul(v3(1.00f, 0.60f, 0.20f), g * 0.32f));
    col = vadd(col, vmul(v3(1.00f, 0.95f, 0.70f), core * 0.80f));
    return col;
}

/* ------------------------------------------------------------------------- */
/* Culling                                                                   */
/* ------------------------------------------------------------------------- */

static void cull_objects(void)
{
    nactive = 0;
    for (int i = 0; i < nobj; i++) {
        vec3 to = vsub(objs[i].pos, cam_pos);
        float dist = sqrtf(vlen2(to));
        if (dist - objs[i].br > MAX_OBJ_DIST) continue;

        float along = vdot(to, cam_f);
        if (along + objs[i].br < -1.5f) continue;

        float lat  = fabsf(vdot(to, cam_r));
        float vert = fabsf(vdot(to, cam_u));
        float cone = along * 1.7f + objs[i].br + 2.0f;
        if (cone < objs[i].br) cone = objs[i].br;

        if (lat > cone || vert > cone) continue;
        active[nactive++] = i;
    }
}

/* ------------------------------------------------------------------------- */
/* Ray marcher                                                               */
/* ------------------------------------------------------------------------- */

static int march(vec3 ro, vec3 rd, float *out_t, int *out_mat, int max_steps, int include_player)
{
    float floor_t = 1e30f;
    if (rd.y < -0.00001f) {
        floor_t = -ro.y / rd.y;
        if (floor_t < 0.0f) floor_t = 1e30f;
    }

    float t = 0.0f;
    for (int i = 0; i < max_steps; i++) {
        if (t >= floor_t) break;

        vec3 p = vadd(ro, vmul(rd, t));
        float d = map_objects(p, include_player);

        if (d < EPS) {
            *out_t = t;
            *out_mat = material_objects(p, include_player);
            return 1;
        }

        if (d < MIN_STEP) d = MIN_STEP;
        t += d;
        if (t > MAX_RAY_DIST) break;
    }

    if (floor_t < MAX_RAY_DIST) {
        *out_t = floor_t;
        *out_mat = -2; /* special floor marker */
        return 1;
    }

    *out_mat = -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

static void render_low(int hq_mode)
{
    const float fov = 1.05f;
    const float aspect = (float)LW / (float)LH;
    const float sx = fov * aspect;

    int steps = hq_mode ? 28 : 18;
    int shsteps = hq_mode ? 5 : 2;

    for (int x = 0; x < LW; x++)
        col_dir[x] = vmul(cam_r, us[x] * sx);

    for (int y = 0; y < LH; y++)
        row_dir[y] = vadd(cam_f, vmul(cam_u, vs[y] * fov));

    for (int y = 0; y < LH; y++) {
        vec3 base = row_dir[y];
        uint8_t *line = lowbuf + y * LW;

        for (int x = 0; x < LW; x++) {
            vec3 d = v3(
                base.x + col_dir[x].x,
                base.y + col_dir[x].y,
                base.z + col_dir[x].z
            );
            vec3 rd = vnorm(d);

            float t;
            int mid;

            /* Primary rays: player is invisible (include_player = 0) */
            if (march(cam_pos, rd, &t, &mid, steps, 0)) {
                vec3 p = vadd(cam_pos, vmul(rd, t));
                vec3 n;

                if (mid == -2) { /* floor */
                    n = v3(0.0f, 1.0f, 0.0f);
                    mid = -1; /* for albedo */
                } else {
                    n = calc_normal(p, 0);
                }

                /* allow_reflect = 1 so mirrors can see the player */
                vec3 col = shade(p, n, rd, mid, shsteps, 1);
                line[x] = rgb_pal(col.x, col.y, col.z, x, y);
            } else {
                vec3 col = sky(rd);
                line[x] = rgb_pal(col.x, col.y, col.z, x, y);
            }
        }
    }
}

static void upscale(uint8_t *bb)
{
    for (int y = 0; y < LH; y++) {
        const uint8_t *src = lowbuf + y * LW;
        uint8_t *dst0 = bb + (y * 2) * OUT_W;
        uint8_t *dst1 = dst0 + OUT_W;

        for (int x = 0; x < LW; x++) {
            uint8_t c = src[x];
            dst0[0] = c; dst0[1] = c;
            dst1[0] = c; dst1[1] = c;
            dst0 += 2;
            dst1 += 2;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* HUD                                                                       */
/* ------------------------------------------------------------------------- */

static void draw_hud(int W, int H)
{
    char buf[64];

    if (edit_mode) {
        gfx_draw_text(4, 2, "EDIT MODE  [Tab=Play]", cube(5, 5, 1), GFX_TRANSPARENT);

        /* object count */
        buf[0] = 'o'; buf[1] = 'b'; buf[2] = 'j'; buf[3] = ' ';
        buf[4] = (char)('0' + (nobj / 10));
        buf[5] = (char)('0' + (nobj % 10));
        buf[6] = '/';
        buf[7] = (char)('0' + (MAX_OBJ / 10));
        buf[8] = (char)('0' + (MAX_OBJ % 10));
        buf[9] = '\0';
        gfx_draw_text(W - 70, 2, buf, cube(4, 4, 5), GFX_TRANSPARENT);

        /* physics indicator */
        if (physics_on)
            gfx_draw_text(W - 90, 14, "PHYS ON", cube(5, 2, 1), GFX_TRANSPARENT);
        else
            gfx_draw_text(W - 90, 14, "PHYS OFF", cube(3, 3, 3), GFX_TRANSPARENT);

        if (selected >= 0 && selected < nobj) {
            Obj *o = &objs[selected];

            /* type + index */
            if (o->type == 1)
                gfx_draw_text(4, 14, "Sphere", cube(5, 5, 5), GFX_TRANSPARENT);
            else
                gfx_draw_text(4, 14, "Cube", cube(5, 5, 5), GFX_TRANSPARENT);

            /* color */
            gfx_draw_text(60, 14, color_names[o->color_id], cube(5, 4, 2), GFX_TRANSPARENT);

            /* material */
            gfx_draw_text(120, 14, mat_names[o->mat_type], cube(5, 3, 5), GFX_TRANSPARENT);

            /* motion (only relevant when physics off) */
            if (!physics_on)
                gfx_draw_text(185, 14, motion_names[o->motion], cube(3, 5, 3), GFX_TRANSPARENT);
            else
                gfx_draw_text(185, 14, "Phys", cube(5, 3, 1), GFX_TRANSPARENT);

            /* size hint */
            buf[0] = 's'; buf[1] = 'z'; buf[2] = ' ';
            int sz10 = (int)(o->size * 10.0f + 0.5f);
            buf[3] = (char)('0' + (sz10 / 10));
            buf[4] = '.';
            buf[5] = (char)('0' + (sz10 % 10));
            buf[6] = '\0';
            gfx_draw_text(235, 14, buf, cube(4, 4, 4), GFX_TRANSPARENT);
        } else {
            gfx_draw_text(4, 14, "No selection  (1=Sphere  2=Cube)", cube(3, 3, 3), GFX_TRANSPARENT);
        }

        gfx_draw_text(4, H - 20,
            "1/2 spawn  Del  </> select  R/G/B color  T material  M motion  +/- size",
            cube(3, 3, 4), GFX_TRANSPARENT);
        gfx_draw_text(4, H - 10,
            "WASD move  Q/E rotate  K=physics  Space pause  F quality",
            cube(3, 3, 4), GFX_TRANSPARENT);
    } else {
        if (physics_on)
            gfx_draw_text(4, 2,
                hq ? "RayMarch EDITOR  HQ  [PHYS]" : "RayMarch EDITOR  FAST  [PHYS]",
                cube(5, 4, 1), GFX_TRANSPARENT);
        else
            gfx_draw_text(4, 2,
                hq ? "RayMarch EDITOR  HQ" : "RayMarch EDITOR  FAST",
                cube(5, 5, 5), GFX_TRANSPARENT);

        buf[0] = 'o'; buf[1] = 'b'; buf[2] = 'j'; buf[3] = ' ';
        buf[4] = (char)('0' + (nactive % 10));
        buf[5] = '/';
        buf[6] = (char)('0' + (nobj % 10));
        buf[7] = '\0';
        gfx_draw_text(W - 56, 2, buf, cube(4, 4, 5), GFX_TRANSPARENT);

        gfx_draw_text(4, H - 10,
            "Tab=Edit  K=Physics  WASD move  mouse look  F quality  Space pause  X exit",
            cube(4, 4, 5), GFX_TRANSPARENT);
    }
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
    gfx_enter_640x480();
    gfx_init_palette();
    gfx_cursor_hide();

    int W = gfx_get_width();
    int H = gfx_get_height();
    uint8_t *bb = gfx_get_backbuffer();

    if (!bb || W != OUT_W || H != OUT_H) {
        gfx_clear(1);
        gfx_draw_text(40, 90, "NEED 640x480 MODE", 15, 1);
        gfx_flip();
        for (int limit = 0; limit < 10000; limit++) {
            int k = kb_getkey();
            if (k == 'x' || k == 'X' || k == 27) break;
            sys_yield();
        }
        gfx_exit_text();
        return 1;
    }

    init_screen_tables();
    reset_cam();

    for (int i = 0; i < 8; i++) {
        (void)mouse_get_dx();
        (void)mouse_get_dy();
    }

    nobj = 0;
    selected = -1;
    edit_mode = 0;
    paused = 0;
    hq = 0;
    physics_on = 0;
    time_f = 0.0f;

    int running = 1;

    while (running) {
        /* mouse look */
        int mdx = mouse_get_dx();
        int mdy = mouse_get_dy();
        mdx = (int)clampf((float)mdx, -50.0f, 50.0f);
        mdy = (int)clampf((float)mdy, -50.0f, 50.0f);
        if (mdx >= -1 && mdx <= 1) mdx = 0;
        if (mdy >= -1 && mdy <= 1) mdy = 0;

        if (mdx) cam_yaw   += (float)mdx * 0.0090f;
        if (mdy) cam_pitch -= (float)mdy * 0.0090f;

        /* key events */
        int key_limit = 32;
        int k;
        while ((k = kb_getkey()) != 0 && key_limit-- > 0) {
            if (k == 'x' || k == 'X' || k == 27) {
                running = 0;
                break;
            }

            if (k == '\t' || k == 9) { /* Tab */
                edit_mode ^= 1;
                continue;
            }

            if (k == ' ') {
                paused ^= 1;
                continue;
            }

            if (k == 'f' || k == 'F') {
                hq ^= 1;
                continue;
            }

            if (k == 'k' || k == 'K') {
                physics_on ^= 1;
                /* when enabling physics, give a small random push so stacked objects separate */
                if (physics_on) {
                    for (int i = 0; i < nobj; i++) {
                        objs[i].vel.x += ((float)((i * 17) % 7) - 3.0f) * 0.4f;
                        objs[i].vel.z += ((float)((i * 13) % 7) - 3.0f) * 0.4f;
                    }
                }
                continue;
            }

            if (k == 'r' || k == 'R') {
                if (edit_mode && selected >= 0)
                    cycle_color(+1); /* R cycles color */
                else
                    reset_cam();
                continue;
            }

            /* ---- Edit mode keys ---- */
            if (edit_mode) {
                if (k == '1') {
                    spawn_object(1); /* sphere */
                    continue;
                }
                if (k == '2') {
                    spawn_object(2); /* cube */
                    continue;
                }

                if (k == 127 || k == 8) { /* Delete / Backspace */
                    delete_selected();
                    continue;
                }

                if (k == 0x4B || k == '[') { /* left */
                    if (nobj > 0) {
                        selected--;
                        if (selected < 0) selected = nobj - 1;
                    }
                    continue;
                }
                if (k == 0x4D || k == ']') { /* right */
                    if (nobj > 0) {
                        selected++;
                        if (selected >= nobj) selected = 0;
                    }
                    continue;
                }

                if (k == 'g' || k == 'G') {
                    cycle_color(+1);
                    continue;
                }
                if (k == 'b' || k == 'B') {
                    cycle_color(-1);
                    continue;
                }

                if (k == 'm' || k == 'M') {
                    cycle_motion();
                    continue;
                }

                if (k == 't' || k == 'T') {
                    cycle_material();
                    continue;
                }

                if (k == '+' || k == '=') {
                    if (selected >= 0) {
                        objs[selected].size = clampf(objs[selected].size + 0.08f, 0.15f, 2.5f);
                    }
                    continue;
                }
                if (k == '-' || k == '_') {
                    if (selected >= 0) {
                        objs[selected].size = clampf(objs[selected].size - 0.08f, 0.15f, 2.5f);
                    }
                    continue;
                }

                if (k == 'q' || k == 'Q') {
                    if (selected >= 0 && objs[selected].type == 2)
                        objs[selected].yaw -= 0.18f;
                    continue;
                }
                if (k == 'e' || k == 'E') {
                    if (selected >= 0 && objs[selected].type == 2)
                        objs[selected].yaw += 0.18f;
                    continue;
                }
            }
        }

        /* continuous keys */
        unsigned st = kb_get_state();

        float sp = edit_mode ? 0.12f : 0.18f;
        float cy = cosf(cam_yaw);
        float sy = sinf(cam_yaw);

        if (edit_mode && selected >= 0) {
            /* move selected object */
            Obj *o = &objs[selected];
            int moved = 0;

            if (st & KB_W) {
                o->base.x += sy * sp;
                o->base.z -= cy * sp;
                moved = 1;
            }
            if (st & KB_S) {
                o->base.x -= sy * sp;
                o->base.z += cy * sp;
                moved = 1;
            }
            if (st & KB_A) {
                o->base.x -= cy * sp;
                o->base.z -= sy * sp;
                moved = 1;
            }
            if (st & KB_D) {
                o->base.x += cy * sp;
                o->base.z += sy * sp;
                moved = 1;
            }
            if (st & KB_UP) {
                o->base.y += sp * 0.7f;
                moved = 1;
            }
            if (st & KB_DOWN) {
                o->base.y -= sp * 0.7f;
                moved = 1;
            }

            if (o->base.y < 0.15f) o->base.y = 0.15f;

            /* if user is dragging the object while physics is on — reset velocity */
            if (moved && physics_on) {
                o->pos = o->base;
                o->vel = v3(0.0f, 0.0f, 0.0f);
            }
        } else {
            /* move camera and track velocity for physics kicks */
            vec3 old_pos = cam_pos;

            if (st & KB_W) {
                cam_pos.x += sy * sp;
                cam_pos.z -= cy * sp;
            }
            if (st & KB_S) {
                cam_pos.x -= sy * sp;
                cam_pos.z += cy * sp;
            }
            if (st & KB_A) {
                cam_pos.x -= cy * sp;
                cam_pos.z -= sy * sp;
            }
            if (st & KB_D) {
                cam_pos.x += cy * sp;
                cam_pos.z += sy * sp;
            }
            if (st & KB_Q) cam_yaw -= 0.06f;
            if (st & KB_E) cam_yaw += 0.06f;
            if (st & KB_UP)   cam_pitch += 0.05f;
            if (st & KB_DOWN) cam_pitch -= 0.05f;

            /* player velocity in units/sec */
            player_vel = vmul(vsub(cam_pos, old_pos), 1.0f / DT);
        }

        if ((st & KB_X) || (st & KB_ESC))
            running = 0;

        if (cam_pos.y < 0.25f)
            cam_pos.y = 0.25f;

        /* decay player_vel when not moving (edit mode or standing still) */
        if (edit_mode) {
            player_vel = v3(0.0f, 0.0f, 0.0f);
        }

        /* animation */
        if (!paused)
            time_f += 0.045f;

        update_object_positions();
        update_cam_basis();
        cull_objects();

        light_dir = vnorm(v3(
            0.40f + 0.15f * cosf(time_f * 0.13f),
            0.85f,
            0.30f + 0.15f * sinf(time_f * 0.13f)
        ));

        render_low(hq);
        upscale(bb);
        draw_hud(W, H);

        gfx_flip();
        sys_yield();
    }

    gfx_exit_text();
    return 0;
}
