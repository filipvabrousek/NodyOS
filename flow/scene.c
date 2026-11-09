/* Flow - scene graph bookkeeping, evaluation, the .flw file format, demo scenes.
 *
 * .flw scene file, little-endian:
 *
 *   0   char[4]  "FLOW"
 *   4   u16      format version (1)
 *   6   u16      node count
 *   8   u16      display node (index into the saved list, 0xFFFF = none)
 *   10  u16      selected node (same)
 *   12  u32      flags: bit 0 loop, bit 1 grid, bit 2 wireframe
 *   16  u32      loop length in frames
 *   20  f32[6]   camera: yaw, pitch, distance, target x, y, z
 *   44  i32[2]   graph editor scroll
 *   52  u32      checksum of every byte from offset 56 to the end
 *   56  node records, each:
 *         char[12] type id ("cube", "cloner", ...), zero padded
 *         i16 x, y   position in the graph editor
 *         u8 flags   bit 0 bypass
 *         u8 np      number of parameters stored
 *         u8 nin     number of inputs stored
 *         u8 0
 *         f32[np]    parameter values
 *         u8[nin]    source node index per input, 0xFF = unconnected
 *         padding to a multiple of 4 bytes
 *
 * Types are stored by name and parameters by count, so files keep loading when
 * node types gain parameters or the type table is reordered.
 */
#include "flow/engine.h"
#include "lib/lib.h"
#include "lib/plat.h"

/* ---- graph ---------------------------------------------------------------- */
static void defaults(Scene *sc)
{
    sc->display = -1;
    sc->selected = -1;
    sc->frame = 0;
    sc->loop_len = 240;
    sc->loop = true;
    sc->grid = true;
    sc->wire = false;
    sc->yaw = 0.62f; sc->pitch = 0.42f; sc->dist = 11.0f;
    sc->target = v3(0, 0.8f, 0);
    sc->pan_x = 0; sc->pan_y = 0;
}

void scene_init(Scene *sc)
{
    memset(sc, 0, sizeof *sc);
    for (int i = 0; i < MAX_NODES; i++)
        for (int k = 0; k < MAX_INPUTS; k++) sc->n[i].in[k] = -1;
    defaults(sc);
}

void scene_clear(Scene *sc)
{
    u32 stamp = sc->stamp;
    for (int i = 0; i < MAX_NODES; i++)
        if (sc->n[i].used && sc->n[i].state) fluid_free(sc->n[i].state);
    scene_init(sc);
    sc->stamp = stamp + 1;              /* never reuse a stamp: cached outputs are gone */
}

int scene_count(const Scene *sc)
{
    int n = 0;
    for (int i = 0; i < MAX_NODES; i++) n += sc->n[i].used;
    return n;
}

int scene_add(Scene *sc, int type, int x, int y)
{
    if (type < 0 || type >= g_ntypes) return -1;
    for (int i = 0; i < MAX_NODES; i++) {
        Node *n = &sc->n[i];
        if (n->used) continue;
        memset(n, 0, sizeof *n);
        n->used = true;
        n->type = (u8)type;
        n->x = x; n->y = y;
        for (int k = 0; k < g_types[type].np; k++) n->p[k] = g_types[type].p[k].def;
        for (int k = 0; k < MAX_INPUTS; k++) n->in[k] = -1;
        return i;
    }
    return -1;
}

void scene_del(Scene *sc, int i)
{
    if (i < 0 || i >= MAX_NODES || !sc->n[i].used) return;
    if (sc->n[i].state) fluid_free(sc->n[i].state);
    sc->n[i].state = NULL;
    sc->n[i].used = false;
    for (int k = 0; k < MAX_NODES; k++)
        for (int s = 0; s < MAX_INPUTS; s++)
            if (sc->n[k].in[s] == i) sc->n[k].in[s] = -1;
    if (sc->display == i) sc->display = -1;
    if (sc->selected == i) sc->selected = -1;
}

/* does node a depend on node b? (depth-first over the inputs, each node once) */
static bool dep_walk(const Scene *sc, int a, int b, bool *seen)
{
    if (a == b) return true;
    if (a < 0 || seen[a]) return false;
    seen[a] = true;
    for (int s = 0; s < MAX_INPUTS; s++)
        if (dep_walk(sc, sc->n[a].in[s], b, seen)) return true;
    return false;
}

static bool depends(const Scene *sc, int a, int b, int unused)
{
    bool seen[MAX_NODES];
    (void)unused;
    memset(seen, 0, sizeof seen);
    return dep_walk(sc, a, b, seen);
}

bool scene_connect(Scene *sc, int src, int dst, int slot)
{
    if (dst < 0 || dst >= MAX_NODES || !sc->n[dst].used) return false;
    if (slot < 0 || slot >= g_types[sc->n[dst].type].nin) return false;
    if (src >= 0) {
        if (src >= MAX_NODES || !sc->n[src].used) return false;
        if (depends(sc, src, dst, 0)) return false;     /* would close a loop */
    }
    sc->n[dst].in[slot] = src;
    return true;
}

/* ---- evaluation ----------------------------------------------------------- */
Geo *node_eval(Scene *sc, int i)
{
    if (i < 0 || i >= MAX_NODES) return NULL;
    Node *n = &sc->n[i];
    if (!n->used) return NULL;
    if (n->stamp == sc->stamp) return n->out;
    if (n->busy) return NULL;
    n->busy = true;
    Geo *g = n->bypass ? node_eval(sc, n->in[0]) : g_types[n->type].eval(sc, n);
    n->busy = false;
    n->out = g;
    n->stamp = sc->stamp;
    return g;
}

Geo *scene_eval(Scene *sc)
{
    arena_reset();
    sc->stamp++;
    if (sc->display < 0 || !sc->n[sc->display].used) return NULL;
    return geo_realize(node_eval(sc, sc->display));
}

/* ---- file format ---------------------------------------------------------- */
#define HDR 56

static u32 checksum(const u8 *p, int n)
{
    u32 a = 1, b = 0;
    for (int i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

int scene_save(const Scene *sc, u8 *buf, int cap)
{
    int map[MAX_NODES], count = 0;
    for (int i = 0; i < MAX_NODES; i++) map[i] = sc->n[i].used ? count++ : -1;
    if (cap < HDR) return 0;
    memset(buf, 0, HDR);
    memcpy(buf, "FLOW", 4);
    put_u16(buf + 4, 1);
    put_u16(buf + 6, (u32)count);
    put_u16(buf + 8, sc->display >= 0 ? (u32)map[sc->display] : 0xFFFF);
    put_u16(buf + 10, sc->selected >= 0 ? (u32)map[sc->selected] : 0xFFFF);
    put_u32(buf + 12, (sc->loop ? 1u : 0u) | (sc->grid ? 2u : 0u) | (sc->wire ? 4u : 0u));
    put_u32(buf + 16, (u32)sc->loop_len);
    put_f32(buf + 20, sc->yaw); put_f32(buf + 24, sc->pitch); put_f32(buf + 28, sc->dist);
    put_f32(buf + 32, sc->target.x); put_f32(buf + 36, sc->target.y); put_f32(buf + 40, sc->target.z);
    put_u32(buf + 44, (u32)sc->pan_x); put_u32(buf + 48, (u32)sc->pan_y);

    int o = HDR;
    for (int i = 0; i < MAX_NODES; i++) {
        const Node *n = &sc->n[i];
        if (!n->used) continue;
        const NodeType *t = &g_types[n->type];
        int size = 12 + 8 + t->np * 4 + ((t->nin + 3) & ~3);
        if (o + size > cap) return 0;
        memset(buf + o, 0, (usize)size);
        str_copy((char *)buf + o, t->id, 12);
        put_u16(buf + o + 12, (u32)(u16)(i16)n->x);
        put_u16(buf + o + 14, (u32)(u16)(i16)n->y);
        buf[o + 16] = n->bypass ? 1 : 0;
        buf[o + 17] = t->np;
        buf[o + 18] = t->nin;
        o += 20;
        for (int k = 0; k < t->np; k++, o += 4) put_f32(buf + o, n->p[k]);
        for (int k = 0; k < t->nin; k++) buf[o + k] = n->in[k] >= 0 ? (u8)map[n->in[k]] : 0xFF;
        o += (t->nin + 3) & ~3;
    }
    put_u32(buf + 52, checksum(buf + HDR, o - HDR));
    return o;
}

bool scene_load(Scene *sc, const u8 *buf, int size)
{
    static Scene tmp;                   /* parse into a scratch scene: a bad file changes nothing */
    if (size < HDR || memcmp(buf, "FLOW", 4) != 0 || get_u16(buf + 4) != 1) return false;
    if (get_u32(buf + 52) != checksum(buf + HDR, size - HDR)) return false;
    int count = (int)get_u16(buf + 6);
    if (count > MAX_NODES) return false;

    scene_init(&tmp);
    u32 flags = get_u32(buf + 12);
    tmp.loop = flags & 1; tmp.grid = (flags & 2) != 0; tmp.wire = (flags & 4) != 0;
    tmp.loop_len = i_clamp((int)get_u32(buf + 16), 1, 100000);
    tmp.yaw = get_f32(buf + 20); tmp.pitch = get_f32(buf + 24); tmp.dist = m_clamp(get_f32(buf + 28), 0.5f, 200.0f);
    tmp.target = v3(get_f32(buf + 32), get_f32(buf + 36), get_f32(buf + 40));
    tmp.pan_x = (int)get_u32(buf + 44); tmp.pan_y = (int)get_u32(buf + 48);

    int o = HDR;
    for (int i = 0; i < count; i++) {
        if (o + 20 > size) return false;
        char id[13];
        memcpy(id, buf + o, 12);
        id[12] = 0;
        int type = type_find(id);
        int np = buf[o + 17], nin = buf[o + 18];
        if (type < 0 || o + 20 + np * 4 + ((nin + 3) & ~3) > size) return false;
        int idx = scene_add(&tmp, type, (i16)get_u16(buf + o + 12), (i16)get_u16(buf + o + 14));
        if (idx != i) return false;
        Node *n = &tmp.n[idx];
        const NodeType *t = &g_types[type];
        n->bypass = buf[o + 16] & 1;
        o += 20;
        for (int k = 0; k < np; k++, o += 4)
            if (k < t->np) n->p[k] = m_clamp(get_f32(buf + o), t->p[k].kind == PK_ENUM ? 0 : t->p[k].lo,
                                             t->p[k].kind == PK_ENUM ? (float)(param_enum_count(&t->p[k]) - 1) : t->p[k].hi);
        for (int k = 0; k < nin; k++)
            if (k < t->nin && buf[o + k] != 0xFF && buf[o + k] < count) n->in[k] = buf[o + k];
        o += (nin + 3) & ~3;
    }
    for (int i = 0; i < count; i++)             /* refuse files that wire a node into its own input chain */
        for (int k = 0; k < MAX_INPUTS; k++) {
            int src = tmp.n[i].in[k];
            if (src < 0) continue;
            tmp.n[i].in[k] = -1;
            if (depends(&tmp, src, i, 0)) return false;
            tmp.n[i].in[k] = src;
        }
    u32 d = get_u16(buf + 8), s = get_u16(buf + 10);
    tmp.display = d < (u32)count ? (int)d : -1;
    tmp.selected = s < (u32)count ? (int)s : -1;

    scene_clear(sc);
    u32 stamp = sc->stamp;
    *sc = tmp;
    sc->stamp = stamp;
    return true;
}

/* ---- demo scenes ---------------------------------------------------------- */
static int add(Scene *sc, const char *id, int x, int y) { return scene_add(sc, type_find(id), x, y); }

static void setp(Scene *sc, int node, const char *name, float v)
{
    if (node < 0) return;
    const NodeType *t = &g_types[sc->n[node].type];
    for (int i = 0; i < t->np; i++)
        if (str_cmp(t->p[i].name, name) == 0) { sc->n[node].p[i] = v; return; }
    kprintf("flow: demo sets unknown parameter %s.%s\n", t->id, name);
}

static const char *const demo_names[] = { "mograph", "fluid", "helix", "voxels", "waves" };

int scene_demo_count(void) { return ARRAY_LEN(demo_names); }
const char *scene_demo_name(int which) { return demo_names[which]; }

void scene_demo(Scene *sc, int which)
{
    scene_clear(sc);
    if (which == 0) {
        /* a field object orbits above a grid of cubes and lifts, scales and tints the ones below it */
        int cube = add(sc, "cube", 30, 30), clo = add(sc, "cloner", 210, 30);
        int sph = add(sc, "sphere", 30, 150), tr = add(sc, "transform", 210, 150), ani = add(sc, "animate", 390, 150);
        int pln = add(sc, "plain", 570, 30), mrg = add(sc, "merge", 750, 80);
        setp(sc, cube, "Size X", 0.8f); setp(sc, cube, "Size Y", 0.8f); setp(sc, cube, "Size Z", 0.8f);
        setp(sc, cube, "Color", (float)0x4A72B0);
        setp(sc, clo, "Mode", 2); setp(sc, clo, "Count X", 11); setp(sc, clo, "Count Z", 11);
        setp(sc, clo, "Space X", 1.0f); setp(sc, clo, "Space Z", 1.0f);
        setp(sc, sph, "Radius", 0.45f); setp(sc, sph, "Color", (float)0xFF8A3D);
        setp(sc, tr, "Move Y", 3.0f);
        setp(sc, ani, "Spin Y", 0); setp(sc, ani, "Orbit Radius", 3.0f); setp(sc, ani, "Orbit Speed", 0.2f);
        setp(sc, pln, "Move Y", 1.5f); setp(sc, pln, "Scale", -0.45f); setp(sc, pln, "Rotate Y", 90);
        setp(sc, pln, "Use Color", 1); setp(sc, pln, "Color", (float)0xFF8A3D);
        setp(sc, pln, "Falloff", 1); setp(sc, pln, "Center Y", -3.0f); setp(sc, pln, "Radius", 3.4f);
        scene_connect(sc, cube, clo, 0);
        scene_connect(sc, sph, tr, 0);
        scene_connect(sc, tr, ani, 0);
        scene_connect(sc, clo, pln, 0);
        scene_connect(sc, ani, pln, 1);
        scene_connect(sc, pln, mrg, 0);
        scene_connect(sc, ani, mrg, 1);
        sc->display = mrg; sc->selected = pln;
        sc->yaw = 0.70f; sc->pitch = 0.50f; sc->dist = 15.0f; sc->target = v3(0, 0.6f, 0);
    } else if (which == 1) {
        /* a jet of fluid pours over a sphere inside a glass tank */
        int sph = add(sc, "sphere", 30, 40), tr = add(sc, "transform", 210, 40);
        int fl = add(sc, "fluid", 390, 20), mrg = add(sc, "merge", 570, 60);
        setp(sc, sph, "Radius", 0.55f); setp(sc, sph, "Segments", 24);
        setp(sc, tr, "Move X", 0.3f); setp(sc, tr, "Move Y", 0.55f);
        scene_connect(sc, sph, tr, 0);
        scene_connect(sc, tr, fl, 0);
        scene_connect(sc, fl, mrg, 0);
        scene_connect(sc, tr, mrg, 1);
        sc->display = mrg; sc->selected = fl;
        sc->yaw = 0.35f; sc->pitch = 0.30f; sc->dist = 7.2f; sc->target = v3(0, 1.0f, 0);
        sc->loop_len = 420;
    } else if (which == 2) {
        /* a spinning ladder of bars: linear cloner with a rotation per step */
        int bar = add(sc, "cube", 30, 40), clo = add(sc, "cloner", 210, 40), wav = add(sc, "wave", 390, 40);
        int col = add(sc, "color", 570, 40), ani = add(sc, "animate", 750, 40);
        setp(sc, bar, "Size X", 2.6f); setp(sc, bar, "Size Y", 0.12f); setp(sc, bar, "Size Z", 0.34f);
        setp(sc, clo, "Mode", 0); setp(sc, clo, "Count", 40);
        setp(sc, clo, "Step X", 0); setp(sc, clo, "Step Y", 0.17f); setp(sc, clo, "Step Rotate", 11);
        setp(sc, wav, "Pattern", 0); setp(sc, wav, "Move Y", 0); setp(sc, wav, "Scale", 0.35f);
        setp(sc, wav, "Frequency", 0.07f); setp(sc, wav, "Speed", 0.5f);
        setp(sc, col, "Mode", 1); setp(sc, col, "Color A", (float)0xFF5D8F); setp(sc, col, "Color B", (float)0x4CC9F0);
        setp(sc, ani, "Spin Y", 30);
        scene_connect(sc, bar, clo, 0);
        scene_connect(sc, clo, wav, 0);
        scene_connect(sc, wav, col, 0);
        scene_connect(sc, col, ani, 0);
        sc->display = ani; sc->selected = clo;
        sc->yaw = 0.5f; sc->pitch = 0.22f; sc->dist = 12.0f; sc->target = v3(0, 3.2f, 0);
    } else if (which == 3) {
        /* a block of fluid dropped into a tank, drawn as cubes cloned onto the particles */
        int cube = add(sc, "cube", 30, 30), fl = add(sc, "fluid", 30, 130), clo = add(sc, "cloner", 230, 60);
        int col = add(sc, "color", 410, 60);
        setp(sc, cube, "Size X", 0.15f); setp(sc, cube, "Size Y", 0.15f); setp(sc, cube, "Size Z", 0.15f);
        setp(sc, fl, "Emitter", 1); setp(sc, fl, "Particles", 600); setp(sc, fl, "Emit X", -0.8f);
        setp(sc, fl, "Emit Y", 1.6f); setp(sc, fl, "Tank X", 3.4f); setp(sc, fl, "Tank Z", 1.2f);
        setp(sc, clo, "Mode", 3);
        setp(sc, col, "Mode", 2); setp(sc, col, "Color A", (float)0x2A6FDB); setp(sc, col, "Color B", (float)0xA8F0FF);
        scene_connect(sc, cube, clo, 0);
        scene_connect(sc, fl, clo, 1);
        scene_connect(sc, clo, col, 0);
        sc->display = col; sc->selected = fl;
        sc->yaw = 0.30f; sc->pitch = 0.28f; sc->dist = 7.0f; sc->target = v3(0, 0.9f, 0);
        sc->loop_len = 300;
    } else {
        /* a subdivided plane pushed around by animated noise, coloured by height */
        int pl = add(sc, "plane", 30, 40), no = add(sc, "noise", 210, 40), col = add(sc, "color", 390, 40);
        setp(sc, pl, "Size X", 9); setp(sc, pl, "Size Z", 9); setp(sc, pl, "Divisions", 44);
        setp(sc, no, "Amount", 0.9f); setp(sc, no, "Scale", 0.55f); setp(sc, no, "Speed", 0.45f);
        setp(sc, col, "Mode", 2); setp(sc, col, "Color A", (float)0x1F4E9E); setp(sc, col, "Color B", (float)0x9BF6E8);
        scene_connect(sc, pl, no, 0);
        scene_connect(sc, no, col, 0);
        sc->display = col; sc->selected = no;
        sc->yaw = 0.60f; sc->pitch = 0.48f; sc->dist = 12.5f; sc->target = v3(0, 0.2f, 0);
        sc->grid = false;
    }
}
