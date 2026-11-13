/* ---- (lowend) video memory: what the port allocates, and the GPU's budget ---
 * Included once by rsx_vulkan_backend.c, after the engine objects, the texture
 * blocks and the recycle pool (it reads them for the [vram] line).
 *
 * Every vkAllocateMemory / vkFreeMemory of the backend goes through the two
 * wrappers below (vk_vram_install swaps them in after the device functions
 * are loaded), and vk_postfx.c's too (it loads them through vk_vram_gdpa):
 * bytes per memory heap, with each allocation's size kept in a small table so
 * a free knows what it gave back. With VK_EXT_memory_budget (enabled when the
 * device has it) the driver also says how much of each heap this process uses
 * and how much it may use (the budget: on Windows the WDDM budget, which
 * shrinks when other programs hold video memory). Past the budget Windows
 * moves allocations to system memory: the heavy frame drops 4 GB cards show.
 *
 * PS3RECOMP_VRAM_LOG=<s> (default 0 = off; 1 = once a second): a [vram] line at
 * the present -- the device-local heap's use and budget, what the backend
 * holds by kind (screen targets, textures in 16 MB blocks and on their own,
 * recycled images, the rest), postfx's own images, and the draw engine's
 * texture cache (entries, guest bytes). The first allocations that fail are
 * logged either way, with the heap's use at that moment.
 *
 * vk_vram_device_local_mb() is what the low-end defaults go by
 * (vk_overlay_lowend_auto, called from vk_init_all); rsx_vk_vram_now() is the
 * menu's "in use now" line; vk_vram_pressure() the reaction near the budget.
 *
 * Measured (2026-11-13, RTX 2070 Super, New Marais walk from the first save,
 * PS3RECOMP_VRAM_LOG=1): the 1080p defaults ~0.9 GB in all (screen targets
 * 434 MB, textures 288 MB in blocks + 14 MB, postfx 28 MB); 720p with the
 * low-end set ~0.65 GB (targets ~200 MB, postfx 13 MB).
 */

#define VK_VRAM_TAB 16384u                    /* power of two; the driver allows ~4096 allocations */
typedef struct { VkDeviceMemory h; VkDeviceSize size; u8 heap, tag; } vk_vram_ent;
enum { VK_VRAM_TAG_BE = 0, VK_VRAM_TAG_PFX = 1, VK_VRAM_TAGS = 2 };
static struct {
    PFN_vkAllocateMemory alloc;               /* the driver's */
    PFN_vkFreeMemory     free_;
    PFN_vkGetPhysicalDeviceMemoryProperties2KHR props2;   /* NULL: no budget numbers */
    int          budget_ok;                   /* VK_EXT_memory_budget enabled */
    int          dl_heap;                     /* the largest DEVICE_LOCAL heap, -1 = none */
    vk_vram_ent  tab[VK_VRAM_TAB];
    char         lock;
    u32          n, lost, fails;
    VkDeviceSize heap_bytes[VK_MAX_MEMORY_HEAPS];
    VkDeviceSize tag_bytes[VK_VRAM_TAGS];     /* device-local only */
    VkDeviceSize peak_dl;                     /* the most device-local bytes held at once */
    long         log_s;                       /* PS3RECOMP_VRAM_LOG, -1 unread */
    u64          next_ns;
} s_vram = { .dl_heap = -1, .log_s = -1 };

static inline void vk_vram_lock(void)   { while (__atomic_test_and_set(&s_vram.lock, __ATOMIC_ACQUIRE)) { } }
static inline void vk_vram_unlock(void) { __atomic_clear(&s_vram.lock, __ATOMIC_RELEASE); }
static inline u32 vk_vram_slot(VkDeviceMemory h)
{
    u64 k = (u64)(uintptr_t)h;
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33;
    return (u32)k & (VK_VRAM_TAB - 1u);
}
static inline int vk_vram_heap_of(u32 type)
{
    return type < s_vk.memprops.memoryTypeCount ? (int)s_vk.memprops.memoryTypes[type].heapIndex : 0;
}
/* The bytes an allocation holds (0 = not one of ours). */
static VkDeviceSize vk_vram_size_of(VkDeviceMemory h)
{
    if (!h) return 0;
    VkDeviceSize sz = 0;
    vk_vram_lock();
    for (u32 p = vk_vram_slot(h); s_vram.tab[p].h; p = (p + 1u) & (VK_VRAM_TAB - 1u))
        if (s_vram.tab[p].h == h) { sz = s_vram.tab[p].size; break; }
    vk_vram_unlock();
    return sz;
}
static void vk_vram_note(VkDeviceMemory h, VkDeviceSize size, u32 type, u8 tag)
{
    const int heap = vk_vram_heap_of(type);
    vk_vram_lock();
    if (s_vram.n >= VK_VRAM_TAB * 3u / 4u) { s_vram.lost++; vk_vram_unlock(); return; }   /* not counted */
    u32 p = vk_vram_slot(h);
    while (s_vram.tab[p].h) p = (p + 1u) & (VK_VRAM_TAB - 1u);
    s_vram.tab[p].h = h; s_vram.tab[p].size = size; s_vram.tab[p].heap = (u8)heap; s_vram.tab[p].tag = tag;
    s_vram.n++;
    s_vram.heap_bytes[heap] += size;
    if (heap == s_vram.dl_heap) {
        s_vram.tag_bytes[tag] += size;
        if (s_vram.heap_bytes[heap] > s_vram.peak_dl) s_vram.peak_dl = s_vram.heap_bytes[heap];
    }
    vk_vram_unlock();
}
static void vk_vram_forget(VkDeviceMemory h)
{
    const u32 mask = VK_VRAM_TAB - 1u;
    vk_vram_lock();
    u32 p = vk_vram_slot(h);
    while (s_vram.tab[p].h && s_vram.tab[p].h != h) p = (p + 1u) & mask;
    if (s_vram.tab[p].h) {
        const vk_vram_ent e = s_vram.tab[p];
        s_vram.heap_bytes[e.heap] -= e.size;
        if (e.heap == s_vram.dl_heap) s_vram.tag_bytes[e.tag] -= e.size;
        s_vram.n--;
        u32 hole = p;                         /* backward shift: no tombstones */
        for (u32 q = (p + 1u) & mask; s_vram.tab[q].h; q = (q + 1u) & mask) {
            const u32 home = vk_vram_slot(s_vram.tab[q].h);
            if (((q - home) & mask) >= ((q - hole) & mask)) { s_vram.tab[hole] = s_vram.tab[q]; hole = q; }
        }
        memset(&s_vram.tab[hole], 0, sizeof s_vram.tab[hole]);
    }
    vk_vram_unlock();
}
static int vk_vram_budget(VkDeviceSize* use, VkDeviceSize* budget);
static VkResult vk_vram_alloc_tag(VkDevice d, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* cb,
                                  VkDeviceMemory* out, u8 tag)
{
    const VkResult r = s_vram.alloc(d, ai, cb, out);
    if (r == VK_SUCCESS && out && *out) {
        vk_vram_note(*out, ai->allocationSize, ai->memoryTypeIndex, tag);
    } else if (r != VK_SUCCESS) {
        const u32 n = __atomic_add_fetch(&s_vram.fails, 1u, __ATOMIC_RELAXED);
        if (n <= 16u || !(n & 255u)) {
            VkDeviceSize use = 0, bud = 0;
            const int b = vk_vram_budget(&use, &bud);
            fprintf(stderr, "[vram] allocation #%u FAILED (VkResult %d): %llu KB in memory type %u (heap %d)%s; "
                    "this game holds %llu MB of the GPU's own memory%s",
                    n, (int)r, (unsigned long long)(ai->allocationSize >> 10), ai->memoryTypeIndex,
                    vk_vram_heap_of(ai->memoryTypeIndex), tag == VK_VRAM_TAG_PFX ? " [postfx]" : "",
                    s_vram.dl_heap >= 0 ? (unsigned long long)(s_vram.heap_bytes[s_vram.dl_heap] >> 20) : 0ull,
                    b ? "" : "\n");
            if (b) fprintf(stderr, "; the driver: %llu MB in use of a %llu MB budget\n",
                           (unsigned long long)(use >> 20), (unsigned long long)(bud >> 20));
        }
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL vk_vram_alloc(VkDevice d, const VkMemoryAllocateInfo* ai,
                                                   const VkAllocationCallbacks* cb, VkDeviceMemory* out)
{ return vk_vram_alloc_tag(d, ai, cb, out, VK_VRAM_TAG_BE); }
static VKAPI_ATTR VkResult VKAPI_CALL vk_vram_alloc_pfx(VkDevice d, const VkMemoryAllocateInfo* ai,
                                                       const VkAllocationCallbacks* cb, VkDeviceMemory* out)
{ return vk_vram_alloc_tag(d, ai, cb, out, VK_VRAM_TAG_PFX); }
static VKAPI_ATTR void VKAPI_CALL vk_vram_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* cb)
{
    if (m) vk_vram_forget(m);
    s_vram.free_(d, m, cb);
}
/* vk_postfx.c's loader: its vkAllocateMemory / vkFreeMemory are counted too. */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_vram_gdpa(VkDevice d, const char* name)
{
    if (s_vram.alloc && name) {
        if (!strcmp(name, "vkAllocateMemory")) return (PFN_vkVoidFunction)vk_vram_alloc_pfx;
        if (!strcmp(name, "vkFreeMemory"))     return (PFN_vkVoidFunction)vk_vram_free;
    }
    return pvkGetDeviceProcAddr(d, name);
}

/* After vkGetPhysicalDeviceMemoryProperties: the heap the GPU's own memory is. */
static void vk_vram_pick_heap(void)
{
    s_vram.dl_heap = -1;
    VkDeviceSize best = 0;
    for (u32 i = 0; i < s_vk.memprops.memoryHeapCount; i++)
        if ((s_vk.memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            s_vk.memprops.memoryHeaps[i].size > best) {
            best = s_vk.memprops.memoryHeaps[i].size; s_vram.dl_heap = (int)i;
        }
}
/* The GPU's own memory in MB (the largest device-local heap), 0 = unknown. */
static u32 vk_vram_device_local_mb(void)
{
    return s_vram.dl_heap >= 0 ? (u32)(s_vk.memprops.memoryHeaps[s_vram.dl_heap].size >> 20) : 0u;
}

static void vk_vram_install(void)
{
    if (s_vram.alloc || !pvkAllocateMemory || !pvkFreeMemory) return;
    s_vram.alloc = pvkAllocateMemory; s_vram.free_ = pvkFreeMemory;
    pvkAllocateMemory = vk_vram_alloc;
    pvkFreeMemory = vk_vram_free;
}

/* The device-local heap: this process's use and its budget (the driver's
 * numbers; 0 = not available). */
static int vk_vram_budget(VkDeviceSize* use, VkDeviceSize* budget)
{
    if (!s_vram.budget_ok || !s_vram.props2 || s_vram.dl_heap < 0) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 p;
    memset(&p, 0, sizeof p);
    p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    p.pNext = &b;
    s_vram.props2(s_vk.phys, &p);
    if (!b.heapBudget[s_vram.dl_heap]) return 0;
    *use = b.heapUsage[s_vram.dl_heap];
    *budget = b.heapBudget[s_vram.dl_heap];
    return 1;
}

/* For the menu (vk_overlay_lowend.inc): the device-local heap's use and budget
 * in MB (1 = the driver gave them) and the card's memory. Any thread. */
int rsx_vk_vram_now(unsigned* use_mb, unsigned* budget_mb, unsigned* card_mb)
{
    VkDeviceSize use = 0, bud = 0;
    const int b = s_vk.device ? vk_vram_budget(&use, &bud) : 0;
    if (use_mb) *use_mb = (unsigned)(use >> 20);
    if (budget_mb) *budget_mb = (unsigned)(bud >> 20);
    if (card_mb) *card_mb = vk_vram_device_local_mb();
    return b;
}

/* The [vram] line's breakdown: what the backend's own tables hold. */
typedef struct {
    VkDeviceSize rt, rt_scaled, tex_own, tex_slots, slab, pool, total_dl, pfx;
    u32 n_rt, n_tex, n_slab_blocks, n_pool;
} vk_vram_parts;
static void vk_vram_collect(vk_vram_parts* v)
{
    memset(v, 0, sizeof *v);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) {
        const vk_eng_obj* o = &s_eobj[i];
        if (o->kind == VK_ENG_COLOR || o->kind == VK_ENG_DEPTH) {
            const VkDeviceSize sz = vk_vram_size_of(o->im.mem);
            v->rt += sz; v->n_rt++;
            if (o->scaled) v->rt_scaled += sz;
        } else if (o->kind == VK_ENG_TEXTURE) {
            v->n_tex++;
            if (o->im.sub) {                   /* a slot: its size class */
                const u32 b = (o->im.sub - 1u) >> 12;
                if (b < s_ts.nblocks) v->tex_slots += 1ull << (VK_TS_MIN_SHIFT + s_ts.bcls[b]);
            } else {
                v->tex_own += vk_vram_size_of(o->im.mem);
            }
        }
    }
    v->n_slab_blocks = s_ts.nblocks;
    v->slab = (VkDeviceSize)s_ts.nblocks * VK_TS_BLOCK;
    for (u32 k = 0; k < s_imgpool_n; k++) v->pool += vk_vram_size_of(s_imgpool[k].mem);
    v->n_pool = s_imgpool_n;
    if (s_vram.dl_heap >= 0) {
        vk_vram_lock();
        v->total_dl = s_vram.heap_bytes[s_vram.dl_heap];
        v->pfx = s_vram.tag_bytes[VK_VRAM_TAG_PFX];
        vk_vram_unlock();
    }
}

/* ---- near the budget: the texture cache stops growing ----------------------
 * Once a second (the budget query is one driver call): past VK_VRAM_HIGH of
 * the budget the draw engine gets a byte limit for its texture cache a tenth
 * below what it holds now (at least VK_VRAM_TEX_FLOOR): new textures then take
 * the place of ones no draw has bound for five seconds, instead of adding to the
 * total (rsx_draw_engine_tex_pressure; never one the scene uses). Still past
 * VK_VRAM_HIGH ten seconds later: another tenth. Below VK_VRAM_LOW: no limit
 * again. PS3RECOMP_VRAM_PRESSURE=1 turns it on (live; default off: the low-end
 * defaults and the menu's "Low-end PC" set it, vk_overlay.cpp).
 * PS3RECOMP_VRAM_BUDGET_MB=<n> (tests): act as if the budget were n MB (the
 * driver's use, this budget). */
#define VK_VRAM_HIGH 0.90
#define VK_VRAM_LOW  0.70
#define VK_VRAM_TEX_FLOOR (256ull << 20)
static struct { volatile int on; long sim_mb; u64 last_ns; u64 limit; u32 steps; int read; } s_vpress;
static int vk_vram_pressure_apply(const char* v)
{
    __atomic_store_n(&s_vpress.on, (v && v[0] == '1') ? 1 : 0, __ATOMIC_RELAXED);
    return 1;
}
static void vk_vram_pressure(u64 now)
{
    if (__builtin_expect(!s_vpress.read, 0)) {
        s_vpress.read = 1;
        vk_vram_pressure_apply(getenv("PS3RECOMP_VRAM_PRESSURE"));
        ps3_setting_register("PS3RECOMP_VRAM_PRESSURE", vk_vram_pressure_apply);
        const char* b = getenv("PS3RECOMP_VRAM_BUDGET_MB");
        s_vpress.sim_mb = (b && *b) ? atol(b) : 0;
        if (s_vpress.sim_mb > 0) fprintf(stderr, "[vram] PS3RECOMP_VRAM_BUDGET_MB=%ld: the budget reaction acts as if "
                                         "the budget were %ld MB\n", s_vpress.sim_mb, s_vpress.sim_mb);
    }
    if (!__atomic_load_n(&s_vpress.on, __ATOMIC_RELAXED)) {
        if (s_vpress.limit) { s_vpress.limit = 0; rsx_draw_engine_tex_pressure(0); }   /* turned off live */
        return;
    }
    VkDeviceSize use = 0, bud = 0;
    if (!vk_vram_budget(&use, &bud)) {
        if (s_vpress.sim_mb <= 0 || s_vram.dl_heap < 0) return;
        use = s_vram.heap_bytes[s_vram.dl_heap];   /* no driver numbers: ours */
    }
    if (s_vpress.sim_mb > 0) bud = (VkDeviceSize)s_vpress.sim_mb << 20;
    if (!bud) return;
    const double r = (double)use / (double)bud;
    unsigned n_tc = 0; unsigned long long tc = 0;
    if (r > VK_VRAM_HIGH && (!s_vpress.limit || now - s_vpress.last_ns >= 10000000000ull)) {
        rsx_draw_engine_tex_stats(&n_tc, &tc);
        u64 lim = (s_vpress.limit && s_vpress.limit < tc ? s_vpress.limit : tc) / 10u * 9u;
        if (lim < VK_VRAM_TEX_FLOOR) lim = VK_VRAM_TEX_FLOOR;
        if (lim != s_vpress.limit) {
            s_vpress.limit = lim; s_vpress.steps++; s_vpress.last_ns = now;
            rsx_draw_engine_tex_pressure(lim);
            fprintf(stderr, "[vram] %llu MB in use of a %llu MB budget (%.0f%%): the texture cache (%llu MB) now "
                    "keeps at most %llu MB, dropping textures not used for a few seconds\n",
                   (unsigned long long)(use >> 20), (unsigned long long)(bud >> 20), 100.0 * r,
                   tc >> 20, (unsigned long long)(lim >> 20));
        }
    } else if (s_vpress.limit && r < VK_VRAM_LOW) {
        s_vpress.limit = 0; s_vpress.last_ns = now;
        rsx_draw_engine_tex_pressure(0);
        fprintf(stderr, "[vram] %llu MB in use of a %llu MB budget (%.0f%%): no texture cache limit again\n",
               (unsigned long long)(use >> 20), (unsigned long long)(bud >> 20), 100.0 * r);
    }
}

/* Once a frame, at the present (the RSX thread). */
static void vk_vram_frame(void)
{
    if (__builtin_expect(s_vram.log_s < 0, 0)) {
        const char* e = getenv("PS3RECOMP_VRAM_LOG");
        s_vram.log_s = (e && *e) ? atol(e) : 0;
        if (s_vram.log_s < 0) s_vram.log_s = 0;
    }
    if (!s_vk.device) return;
    const u64 now = ps3_ft_now();
    if (now < s_vram.next_ns) return;
    const int first = s_vram.next_ns == 0;
    s_vram.next_ns = now + (u64)(s_vram.log_s > 0 ? s_vram.log_s : 1) * 1000000000ull;
    if (first) return;                        /* the clock starts at the first present */
    vk_vram_pressure(now);
    if (!s_vram.log_s) return;
    vk_vram_parts v;
    vk_vram_collect(&v);
    unsigned n_tc = 0; unsigned long long tc_bytes = 0, nd = 0, ndb = 0;
    rsx_draw_engine_tex_stats(&n_tc, &tc_bytes);
    rsx_draw_engine_tex_dropped(&nd, &ndb);
    const unsigned long long tlim = rsx_draw_engine_tex_limit();
    VkDeviceSize use = 0, bud = 0;
    const int b = vk_vram_budget(&use, &bud);
    const VkDeviceSize be = v.total_dl - v.pfx;
    const VkDeviceSize known = v.rt + v.tex_own + v.slab + v.pool;
    VkDeviceSize host = 0;
    vk_vram_lock();
    for (u32 i = 0; i < s_vk.memprops.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        if ((int)i != s_vram.dl_heap) host += s_vram.heap_bytes[i];
    vk_vram_unlock();
    char drv[96] = "no budget numbers (VK_EXT_memory_budget)";
    if (b) snprintf(drv, sizeof drv, "driver: %llu MB in use of a %llu MB budget (%.0f%%)",
                    (unsigned long long)(use >> 20), (unsigned long long)(bud >> 20),
                    100.0 * (double)use / (double)bud);
    char lim[64] = "no limit";
    if (tlim) snprintf(lim, sizeof lim, "limit %llu MB", tlim >> 20);
    fprintf(stderr,
            "[vram] frame %u: GPU memory %u MB, %s; this backend %llu MB (peak %llu): targets %llu MB (%u, %llu MB "
            "scaled), textures %llu MB in %u blocks (slots in use %llu MB) + %llu MB own (%u textures), recycled "
            "images %llu MB (%u), other %llu MB; postfx %llu MB; system memory %llu MB; texture cache %u entries, "
            "%llu MB guest bytes, %s, dropped %llu (%llu MB); %u allocations, %u failed\n",
            s_vk.presented, vk_vram_device_local_mb(), drv,
            (unsigned long long)(be >> 20), (unsigned long long)(s_vram.peak_dl >> 20),
            (unsigned long long)(v.rt >> 20), v.n_rt, (unsigned long long)(v.rt_scaled >> 20),
            (unsigned long long)(v.slab >> 20), v.n_slab_blocks, (unsigned long long)(v.tex_slots >> 20),
            (unsigned long long)(v.tex_own >> 20), v.n_tex,
            (unsigned long long)(v.pool >> 20), v.n_pool,
            (unsigned long long)((be > known ? be - known : 0) >> 20),
            (unsigned long long)(v.pfx >> 20), (unsigned long long)(host >> 20),
            n_tc, tc_bytes >> 20, lim, nd, ndb >> 20, s_vram.n, s_vram.fails);
}
