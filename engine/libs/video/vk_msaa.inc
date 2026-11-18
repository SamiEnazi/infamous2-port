/*
 * ps3recomp - [aa] MSAA for the game's screen targets (PS3RECOMP_MSAA).
 * Included by rsx_vulkan_backend.c after vk_eng_render_pass; the state
 * (s_ms) and the target objects' fields (vk_eng_obj ms, ms_elig, ms_ok,
 * ms_fb, ms_set) are declared there.
 *
 * PS3RECOMP_MSAA=off|2|4|8 (read at start; default off). With it on:
 *   - every colour and depth target of the guest's display size (1280x720 in
 *     guest pixels: the scene, the G-buffer planes, their depth; at any
 *     internal resolution) is "eligible" (ms_elig). Shadow maps, the 512x288
 *     effects target, the glow chain and the backend's own pass outputs are
 *     not: they stay 1x, as before;
 *   - an engine render pass whose colour targets and depth are all eligible
 *     draws into each one's multisampled twin (ms, made at its first such
 *     pass) and resolves into the target's own image (`im`) at its end:
 *     colour by the average (render-pass resolve attachments), stencil by
 *     sample 0, depth by vk_ms_zresolve (VK_KHR_depth_stencil_resolve).
 *     `im` stays what
 *     every reader reads -- textures, the depth decodes, snapshots, copies,
 *     readbacks to the guest, the GPU light / SSAO passes, the present -- so
 *     none of them changes;
 *   - a twin "holds the picture" (ms_ok) after a full clear (clears go to
 *     both images), or for colour after a copy of `im` drawn into every
 *     sample (vk_ms_unresolve). A pass that cannot be multisampled (a target
 *     not eligible, a draw reading its targets as input attachments, a depth
 *     twin not cleared yet) draws 1x into `im` as before; what it writes
 *     marks those twins stale (the depth one until the next clear, a colour
 *     one until its next copy);
 *   - pipelines: vk_idx bit 2 = the MSAA variant (this render pass, s_ms.n
 *     samples, specialisation constant 200 = s_ms.n for the alpha-coverage
 *     test of rsx_fp_apply_alpha_aa).
 * Off (or no device support): s_ms.n = 1, no device extension is asked for,
 * no object is eligible, every pass and pipeline is the old one.
 * Logs: one startup line ([aa] MSAA ...), loud first occurrences, and an
 * [aa] stats line every 10 s (passes / draws multisampled vs 1x and why,
 * twins made and their memory, colour copies, stale marks, the alpha-test
 * programs rewritten).
 */

static void vk_ms_read(void)
{
    if (s_ms.read) return;
    s_ms.read = 1;
    const char* e = getenv("PS3RECOMP_MSAA");
    u32 w = 1u;
    if (e && *e && strcmp(e, "off") && strcmp(e, "0")) {
        const long v = atol(e);                 /* "4", "4x" */
        w = v >= 8 ? 8u : v >= 4 ? 4u : v >= 2 ? 2u : 1u;
    }
    s_ms.want = w;
}

/* Before vkCreateDevice: the extensions MSAA needs, at the front of `out`
 * (0 when off or not offered: the device as before). props2: the instance
 * has VK_KHR_get_physical_device_properties2 (VK_KHR_multiview needs it). */
static u32 vk_ms_device_exts(const char** out, int props2)
{
    static const char* const ex[4] = {
        VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE2_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME };
    vk_ms_read();
    if (s_ms.want < 2u) return 0;
    if (!props2) {
        VK_LOG("[aa] WARNING: MSAA %ux asked, but the Vulkan instance has no properties2: MSAA off\n", s_ms.want);
        return 0;
    }
    for (u32 k = 0; k < 4; k++)
        if (!vk_has_device_extension(s_vk.phys, ex[k])) {
            VK_LOG("[aa] WARNING: MSAA %ux asked, but the driver has no %s: MSAA off\n", s_ms.want, ex[k]);
            return 0;
        }
    for (u32 k = 0; k < 4; k++) out[k] = ex[k];
    s_ms.exts = 1;
    return 4;
}

/* After the device and its functions: the sample count in effect. */
static void vk_ms_device_ready(void)
{
    if (s_ms.want < 2u) return;
    if (!s_ms.exts) return;   /* said why */
    s_ms.crp2 = (PFN_vkCreateRenderPass2KHR)(void*)pvkGetDeviceProcAddr(s_vk.device, "vkCreateRenderPass2KHR");
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    VkSampleCountFlags c = p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts;
    if (VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT) c &= p.limits.framebufferStencilSampleCounts;
    u32 n = s_ms.want;
    while (n > 1u && !(c & n)) n >>= 1;
    if (!s_ms.crp2 || n < 2u) {
        VK_LOG("[aa] WARNING: MSAA %ux asked, but %s: MSAA off\n", s_ms.want,
               !s_ms.crp2 ? "vkCreateRenderPass2KHR is missing" : "the GPU offers no such sample count");
        return;
    }
    s_ms.n = n;
    s_ms.bits = (VkSampleCountFlagBits)n;
    VK_LOG("[aa] MSAA %ux on%s (PS3RECOMP_MSAA): the display-size targets draw multisampled and resolve at each "
           "pass's end (colour: average, depth: PS3RECOMP_MSAA_ZRESOLVE, stencil: sample 0)\n", n, n != s_ms.want ? " (fewer than asked)" : "");
}

/* [fix07] The resolved depth of a multisampled pass (PS3RECOMP_MSAA_ZRESOLVE=min|max|avg|zero;
 * default min = the nearest sample). The resolved depth is what every 1x pass
 * tests against, what a stale twin is refilled from and what the full-screen
 * passes (fog, light, SSAO) read as a texture. With sample 0 (feat/aa's first
 * choice), which is not at the pixel centre, two things went wrong (Sami,
 * next/0.7 with MSAA 4x; FIX07.md in work/native):
 *  - the game re-draws its skin layers over depth an earlier pass wrote; the
 *    re-draw of the SAME triangle passed or failed by the triangle's depth
 *    slope: whole triangles of Cole's face and hands lighter or darker;
 *  - at the outline of far buildings the pixel's colour is a mix of building
 *    and sky, but the fog pass saw only sample 0's depth: dotted dark
 *    outlines at the horizon.
 * Measured on the dock cutscene and the harbour view: min = smooth skin, clean
 * outlines, whole rigging lines; max = smooth skin, outlines stay; avg = clean
 * outlines, a few skin triangles left; zero = both faults. Falls back to
 * sample 0 when the driver lacks the mode or cannot resolve depth and stencil
 * differently. */
static VkResolveModeFlagBits vk_ms_zresolve(void)
{
    static int mode = -1;
    if (mode >= 0) return (VkResolveModeFlagBits)mode;
    const char* e = getenv("PS3RECOMP_MSAA_ZRESOLVE");
    VkResolveModeFlagBits want = VK_RESOLVE_MODE_MIN_BIT;
    if (e && !strcmp(e, "zero")) want = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    else if (e && !strcmp(e, "max")) want = VK_RESOLVE_MODE_MAX_BIT;
    else if (e && !strcmp(e, "avg")) want = VK_RESOLVE_MODE_AVERAGE_BIT;
    VkPhysicalDeviceDepthStencilResolveProperties rp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &rp };
    PFN_vkGetPhysicalDeviceProperties2KHR gp2 = (PFN_vkGetPhysicalDeviceProperties2KHR)
        pvkGetInstanceProcAddr(s_vk.instance, "vkGetPhysicalDeviceProperties2KHR");
    if (!gp2) gp2 = (PFN_vkGetPhysicalDeviceProperties2KHR)
        pvkGetInstanceProcAddr(s_vk.instance, "vkGetPhysicalDeviceProperties2");
    if (gp2) gp2(s_vk.phys, &p2);
    const int st = (VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    VkResolveModeFlagBits got = want;
    if (want != VK_RESOLVE_MODE_SAMPLE_ZERO_BIT &&
        (!gp2 || !(rp.supportedDepthResolveModes & want) || (st && !rp.independentResolve)))
        got = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    VK_LOG("[aa] MSAA depth resolve: %s%s (PS3RECOMP_MSAA_ZRESOLVE=min|max|avg|zero; driver modes 0x%X, independent %d)\n",
           got == VK_RESOLVE_MODE_MAX_BIT ? "the farthest sample (max)" : got == VK_RESOLVE_MODE_MIN_BIT ? "the nearest sample (min)" :
           got == VK_RESOLVE_MODE_AVERAGE_BIT ? "the average" : "sample 0",
           got != want ? " -- WARNING: the mode asked for is not offered by the driver" : "",
           (unsigned)rp.supportedDepthResolveModes, (int)rp.independentResolve);
    mode = (int)got;
    return got;
}

/* The MSAA twin of vk_eng_render_pass(n): n colours + depth at s_ms.n
 * samples, LOAD/STORE in GENERAL as everywhere here, resolved into the
 * targets' own images (attachments n+1 .. 2n+1). No input attachments: a
 * draw reading its targets that way is drawn 1x. */
static VkRenderPass vk_ms_render_pass(u32 n)
{
    if (s_ms.n < 2u || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_ms.rp[n]) return s_ms.rp[n];
    VkAttachmentDescription2 att[2 * RSX_BE_MAX_COLOR_TARGETS + 2];
    VkAttachmentReference2 cref[RSX_BE_MAX_COLOR_TARGETS], rref[RSX_BE_MAX_COLOR_TARGETS], dref, drref;
    const int st = (VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    for (u32 k = 0; k < 2u; k++) {               /* k 0: the multisampled set, 1: the resolve targets */
        const u32 base = k ? n + 1u : 0u;
        for (u32 i = 0; i <= n; i++) {
            const int z = i == n;
            att[base + i] = (VkAttachmentDescription2){
                .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                .format = z ? s_vk.depth_format : VK_FORMAT_R8G8B8A8_UNORM,
                .samples = k ? VK_SAMPLE_COUNT_1_BIT : s_ms.bits,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = (z && st) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = (z && st) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        }
    }
    for (u32 i = 0; i < n; i++) {
        cref[i] = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                                            .attachment = i, .layout = VK_IMAGE_LAYOUT_GENERAL };
        rref[i] = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                                            .attachment = n + 1u + i, .layout = VK_IMAGE_LAYOUT_GENERAL };
    }
    dref  = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                                      .attachment = n, .layout = VK_IMAGE_LAYOUT_GENERAL };
    drref = (VkAttachmentReference2){ .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                                      .attachment = 2u * n + 1u, .layout = VK_IMAGE_LAYOUT_GENERAL };
    const VkSubpassDescriptionDepthStencilResolve dsr = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .depthResolveMode = vk_ms_zresolve(),
        .stencilResolveMode = st ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE,
        .pDepthStencilResolveAttachment = &drref };
    const VkSubpassDescription2 sub = {
        .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, .pNext = &dsr,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = n, .pColorAttachments = cref, .pResolveAttachments = rref,
        .pDepthStencilAttachment = &dref };
    const VkRenderPassCreateInfo2 rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
        .attachmentCount = 2u * n + 2u, .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub };
    if (s_ms.crp2(s_vk.device, &rci, NULL, &s_ms.rp[n]) != VK_SUCCESS) {
        s_ms.rp[n] = VK_NULL_HANDLE;
        static int said;
        if (!said++) VK_LOG("[aa] WARNING: the MSAA render pass for %u colour target(s) failed: those passes stay 1x\n", n);
    }
    return s_ms.rp[n];
}

/* The full mask a twin holds the picture with: colour 1; depth 1 (+ 2 stencil). */
static u8 vk_ms_full(const vk_eng_obj* o)
{
    return (o->kind == VK_ENG_DEPTH && (VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT)) ? 3u : 1u;
}

/* A multisampled image of `fmt`, never pooled (w stays 0: vk_destroy_image_now
 * frees it). Device-local. */
static int vk_ms_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h, VkImageUsageFlags usage,
                              VkImageAspectFlags aspect)
{
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = s_ms.bits, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (pvkCreateImage(s_vk.device, &ici, NULL, &im->img) != VK_SUCCESS) { im->img = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        pvkDestroyImage(s_vk.device, im->img, NULL); im->img = VK_NULL_HANDLE; return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem) != VK_SUCCESS) {
        pvkDestroyImage(s_vk.device, im->img, NULL); memset(im, 0, sizeof *im); return -1;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    if (pvkBindImageMemory(s_vk.device, im->img, im->mem, 0) != VK_SUCCESS ||
        (vci.image = im->img, pvkCreateImageView(s_vk.device, &vci, NULL, &im->view) != VK_SUCCESS)) {
        pvkDestroyImage(s_vk.device, im->img, NULL); pvkFreeMemory(s_vk.device, im->mem, NULL);
        memset(im, 0, sizeof *im); return -1;
    }
    im->fmt = fmt; im->usage = usage; im->aspect = aspect; im->levels = 1;   /* w = 0: not pooled */
    s_ms.made_mb += (double)req.size / (1024.0 * 1024.0);
    return 0;
}

/* The twin of target `o` (outside a render pass; batched, it joins the
 * batch): defined contents (cleared), not yet the target's picture. */
static int vk_ms_twin(vk_eng_obj* o)
{
    if (o->ms.img) return 0;
    const int z = o->kind == VK_ENG_DEPTH;
    if (vk_ms_create_image(&o->ms, o->fmt, o->w, o->h,
                           z ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                             : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
                           z ? VK_DS_ASPECT : VK_IMAGE_ASPECT_COLOR_BIT)) {
        o->ms_elig = 0;                          /* not again: this target stays 1x */
        static int said;
        if (said++ < 4) VK_LOG("[aa] WARNING: no %ux%u %ux multisampled image (out of video memory?): that target stays 1x\n",
                               o->w, o->h, s_ms.n);
        return -1;
    }
    if (vk_begin()) return -1;
    vk_barrier_image(o->ms.img, z ? VK_DS_ASPECT : VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (z) {
        const VkClearDepthStencilValue far_plane = { 1.0f, 0 };
        const VkImageSubresourceRange r = { VK_DS_ASPECT, 0, 1, 0, 1 };
        pvkCmdClearDepthStencilImage(s_vk.cmd, o->ms.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    } else {
        const VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
        const VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pvkCmdClearColorImage(s_vk.cmd, o->ms.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    }
    if (!s_batch.recording && vk_submit_and_wait()) return -1;
    o->ms_ok = 0;
    o->ms_dw = (u16)o->w; o->ms_dh = (u16)o->h;   /* all of it */
    s_ms.made++;
    if (s_ms.made == 1)
        VK_LOG("[aa] first multisampled target: %ux%u %s, %ux\n", o->w, o->h, z ? "depth" : "colour", s_ms.n);
    return 0;
}

static void vk_ms_obj_release(vk_eng_obj* o)
{
    if (o->ms_fb) { vk_batch_retire_fb(o->ms_fb); o->ms_fb = VK_NULL_HANDLE; }
    o->ms_set = VK_NULL_HANDLE;                  /* the pool's; reset with it */
    if (o->ms.img || o->ms.view) vk_destroy_image(&o->ms);
    o->ms_ok = 0; o->ms_dw = o->ms_dh = 0;
}

/* Clears (inside vk_eng_clear_*, the command buffer open): the twin too. */
static void vk_ms_clear_color(vk_eng_obj* o, const VkClearColorValue* v)
{
    const VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_barrier_image(o->ms.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->ms.img, VK_IMAGE_LAYOUT_GENERAL, v, 1, &r);
    o->ms_ok = 1; o->ms_dw = o->ms_dh = 0;
}
static void vk_ms_clear_depth(vk_eng_obj* o, VkImageAspectFlags a, const VkClearDepthStencilValue* d)
{
    const VkImageSubresourceRange r = { a, 0, 1, 0, 1 };
    vk_barrier_image(o->ms.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->ms.img, VK_IMAGE_LAYOUT_GENERAL, d, 1, &r);
    o->ms_ok |= (u8)(((a & VK_IMAGE_ASPECT_DEPTH_BIT) ? 1u : 0u) | ((a & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2u : 0u));
    if (o->ms_ok == vk_ms_full(o)) o->ms_dw = o->ms_dh = 0;
}

static void vk_ms_drop_framebuffers(void)
{
    if (s_ms.fb_count && s_batch.recording) vk_batch_end_pass();
    for (u32 i = 0; i < s_ms.fb_count; i++)
        if (s_ms.fb[i].fb) vk_batch_retire_fb(s_ms.fb[i].fb);
    s_ms.fb_count = 0;
}

/* Depth for an MSAA pass that names none (or one too small): a scratch twin
 * of vk_eng_scratch_depth, which it resolves into. */
static VkImageView vk_ms_scratch(u32 w, u32 h)
{
    if (s_ms.scratch.view && s_ms.scratch_w >= w && s_ms.scratch_h >= h) return s_ms.scratch.view;
    vk_eng_drop_framebuffers();
    if (s_ms.scratch.img) vk_destroy_image(&s_ms.scratch);
    if (vk_ms_create_image(&s_ms.scratch, s_vk.depth_format, w, h,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_DS_ASPECT) ||
        vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_ms.scratch.img, VK_DS_ASPECT, VK_IMAGE_LAYOUT_UNDEFINED);
    const VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    const VkImageSubresourceRange r = { VK_DS_ASPECT, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_ms.scratch.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (!s_batch.recording && vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_ms.scratch_w = w; s_ms.scratch_h = h;
    return s_ms.scratch.view;
}

/* The MSAA pass's framebuffer: the twins, then the targets' own images (the
 * resolves). zo: the depth target, NULL = the scratch pair. */
static VkFramebuffer vk_ms_framebuffer(VkRenderPass rp, const u32* rt, u32 n, const vk_eng_obj* zo, u32 zh,
                                       u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = zo ? zh : 0xFFFFFFFFu;
    for (u32 i = 0; i < s_ms.fb_count; i++)
        if (!memcmp(s_ms.fb[i].key, key, sizeof key)) return s_ms.fb[i].fb;
    VkImageView zms = VK_NULL_HANDLE, zres = VK_NULL_HANDLE;
    if (zo) { zms = zo->ms.view; zres = zo->im.view; }
    else {
        zres = vk_eng_scratch_depth(w, h);       /* may drop the framebuffers: before the cache fills */
        zms = zres ? vk_ms_scratch(w, h) : VK_NULL_HANDLE;
    }
    if (!zms || !zres) return VK_NULL_HANDLE;
    if (s_ms.fb_count >= VK_MS_FB_CACHE) vk_ms_drop_framebuffers();
    VkImageView views[2 * RSX_BE_MAX_COLOR_TARGETS + 2];
    for (u32 i = 0; i < n; i++) { views[i] = s_eobj[rt[i]].ms.view; views[n + 1u + i] = s_eobj[rt[i]].im.view; }
    views[n] = zms; views[2u * n + 1u] = zres;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = 2u * n + 2u, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_ms.fb[s_ms.fb_count].key, key, sizeof key);
    s_ms.fb[s_ms.fb_count++].fb = fb;
    return fb;
}

/* ---- the colour copy im -> twin (every sample the pixel's value) ---- */
static const char kVkMsCopyFS[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = texelFetch(src, ivec2(gl_FragCoord.xy), 0); }\n";
static VkShaderModule vk_pass_module(const char* glsl, int stage, const char* what);
static int vk_ms_copy_init(void)
{
    if (s_ms.upipe) return 0;
    if (s_ms.ufailed) return -1;
    s_ms.ufailed = 1;
    const VkAttachmentDescription att = {
        .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = s_ms.bits,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    const VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    const VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       .colorAttachmentCount = 1, .pColorAttachments = &ref };
    const VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                         .attachmentCount = 1, .pAttachments = &att,
                                         .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_ms.urp) != VK_SUCCESS) return -1;
    const VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    const VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 1, .pBindings = &b };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_ms.ulay) != VK_SUCCESS) return -1;
    const VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1, .pSetLayouts = &s_ms.ulay };
    if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_ms.upl) != VK_SUCCESS) return -1;
    const VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_MS_SETS };
    const VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .maxSets = VK_MS_SETS, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_ms.upool) != VK_SUCCESS) return -1;
    if (!(s_ms.uvs = vk_pass_module(kVkPassVS, RSX_SHADER_STAGE_VERTEX, "[aa] copy vertex")) ||
        !(s_ms.ufs = vk_pass_module(kVkMsCopyFS, RSX_SHADER_STAGE_FRAGMENT, "[aa] copy fragment")))
        return -1;
    const VkPipelineShaderStageCreateInfo stg[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_ms.uvs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_ms.ufs, .pName = "main" } };
    const VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    const VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    const VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    const VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    const VkPipelineMultisampleStateCreateInfo msi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = s_ms.bits };
    const VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xFu };
    const VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    const VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stg, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &msi,
        .pColorBlendState = &cb, .pDynamicState = &dy, .layout = s_ms.upl, .renderPass = s_ms.urp, .subpass = 0 };
    if (pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &s_ms.upipe) != VK_SUCCESS) {
        s_ms.upipe = VK_NULL_HANDLE;
        return -1;
    }
    s_ms.ufailed = 0;
    return 0;
}

/* Colour twin <- the target's own picture, each sample the pixel's value
 * (outside a render pass). 0: the twin holds the picture. */
static int vk_ms_unresolve(vk_eng_obj* o)
{
    if (vk_ms_copy_init()) {
        static int said;
        if (!said++) VK_LOG("[aa] WARNING: the colour copy into a multisampled target cannot be built: "
                            "a target drawn 1x stays 1x until its next clear\n");
        s_ms.unres_fail++;
        return -1;
    }
    if (!o->ms_set) {
        if (s_ms.usets >= VK_MS_SETS) {          /* every set any recorded copy may read: done, then all anew */
            vk_batch_flush();
            vk_fif_wait_other();
            pvkResetDescriptorPool(s_vk.device, s_ms.upool, 0);
            for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
                if (s_eobj[i].kind == VK_ENG_COLOR) s_eobj[i].ms_set = VK_NULL_HANDLE;
            s_ms.usets = 0;
        }
        const VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                  .descriptorPool = s_ms.upool, .descriptorSetCount = 1,
                                                  .pSetLayouts = &s_ms.ulay };
        if (pvkAllocateDescriptorSets(s_vk.device, &dai, &o->ms_set) != VK_SUCCESS) {
            o->ms_set = VK_NULL_HANDLE; s_ms.unres_fail++; return -1;
        }
        s_ms.usets++;
        const VkDescriptorImageInfo ii = { s_vk.sampler, o->im.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = o->ms_set,
                                          .dstBinding = 0, .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .pImageInfo = &ii };
        pvkUpdateDescriptorSets(s_vk.device, 1, &wr, 0, NULL);
    }
    if (!o->ms_fb) {
        const VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                              .renderPass = s_ms.urp, .attachmentCount = 1, .pAttachments = &o->ms.view,
                                              .width = o->w, .height = o->h, .layers = 1 };
        if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &o->ms_fb) != VK_SUCCESS) {
            o->ms_fb = VK_NULL_HANDLE; s_ms.unres_fail++; return -1;
        }
    }
    if (vk_begin()) return -1;
    vk_barrier_all();                            /* after whatever wrote `im` */
    /* only the corner the 1x draws wrote since the twin last held the picture */
    const u32 cw = (o->ms_dw && o->ms_dw < o->w) ? o->ms_dw : o->w, ch = (o->ms_dh && o->ms_dh < o->h) ? o->ms_dh : o->h;
    const VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = s_ms.urp, .framebuffer = o->ms_fb,
                                        .renderArea = { { 0, 0 }, { cw, ch } } };
    vk_gprof_stamp(GP_BGAP, o->w, o->h);
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ms.upipe);
    const VkViewport vp = { 0.0f, 0.0f, (float)o->w, (float)o->h, 0.0f, 1.0f };
    const VkRect2D sc = { { 0, 0 }, { o->w, o->h } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ms.upl, 0, 1, &o->ms_set, 0, NULL);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_gprof_stamp(GP_BPASS, cw, ch);
    s_ecs.valid = 0;                             /* the state filter's view of the command buffer is gone */
    if (!s_batch.recording && vk_submit_and_wait()) return -1;
    o->ms_ok = 1; o->ms_dw = o->ms_dh = 0;
    s_ms.unres++;
    return 0;
}

/* ---- the depth copy im -> twin: depth through gl_FragDepth, stencil a bit
 * at a time (no stencil export in Vulkan 1.0): cleared to 0 by the pass,
 * then for each bit a draw that keeps only the pixels with that bit and
 * writes it through a one-bit write mask. Reads the target in place
 * (PS3RECOMP_VK_ZDIRECT's depth and stencil views); without them a stale
 * depth twin waits for the next clear. PS3RECOMP_MSAA_ZCOPY=0: never. */
static const char kVkMsZCopyFS[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D zsrc;\n"
    "void main() { gl_FragDepth = texelFetch(zsrc, ivec2(gl_FragCoord.xy), 0).r; }\n";
static const char kVkMsSCopyFS[] =
    "#version 450\n"
    "layout(set = 0, binding = 1) uniform usampler2D ssrc;\n"
    "layout(push_constant) uniform PC { uint bit; } pc;\n"
    "void main() { if (((texelFetch(ssrc, ivec2(gl_FragCoord.xy), 0).r >> pc.bit) & 1u) == 0u) discard; }\n";
static struct {
    int  tried, ok, on;
    VkRenderPass rp;
    VkDescriptorSetLayout lay;
    VkPipelineLayout pl;
    VkDescriptorPool pool;
    VkShaderModule zfs, sfs;
    VkPipeline zpipe, spipe[8];
    u32  sets;
    unsigned long long copies;
} s_mz;
static int vk_ms_zcopy_init(void)
{
    if (s_mz.tried) return s_mz.ok ? 0 : -1;
    s_mz.tried = 1;
    const char* e = getenv("PS3RECOMP_MSAA_ZCOPY");
    if ((e && e[0] == '0') || !s_ropt.zdirect || vk_ms_copy_init()) return -1;   /* uvs: the copy's vertex module */
    const int st = (VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    const VkAttachmentDescription att = {
        .format = s_vk.depth_format, .samples = s_ms.bits,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = st ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = st ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    const VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    const VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       .pDepthStencilAttachment = &ref };
    const VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                         .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_mz.rp) != VK_SUCCESS) return -1;
    const VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    const VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 2, .pBindings = b };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_mz.lay) != VK_SUCCESS) return -1;
    const VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    const VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1, .pSetLayouts = &s_mz.lay,
                                              .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_mz.pl) != VK_SUCCESS) return -1;
    const VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * VK_MS_SETS };
    const VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .maxSets = VK_MS_SETS, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_mz.pool) != VK_SUCCESS) return -1;
    if (!(s_mz.zfs = vk_pass_module(kVkMsZCopyFS, RSX_SHADER_STAGE_FRAGMENT, "[aa] depth copy")) ||
        (st && !(s_mz.sfs = vk_pass_module(kVkMsSCopyFS, RSX_SHADER_STAGE_FRAGMENT, "[aa] stencil copy"))))
        return -1;
    VkPipelineShaderStageCreateInfo stg[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_ms.uvs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_mz.zfs, .pName = "main" } };
    const VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    const VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    const VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    const VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    const VkPipelineMultisampleStateCreateInfo msi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = s_ms.bits };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS };
    const VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stg, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &msi, .pDepthStencilState = &ds,
        .pColorBlendState = &cb, .pDynamicState = &dy, .layout = s_mz.pl, .renderPass = s_mz.rp, .subpass = 0 };
    if (pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &s_mz.zpipe) != VK_SUCCESS) return -1;
    if (st) {
        stg[1].module = s_mz.sfs;
        ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
        ds.stencilTestEnable = VK_TRUE;
        for (u32 k = 0; k < 8; k++) {
            ds.front = (VkStencilOpState){ .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
                                           .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
                                           .compareMask = 0xFFu, .writeMask = 1u << k, .reference = 0xFFu };
            ds.back = ds.front;
            if (pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &s_mz.spipe[k]) != VK_SUCCESS)
                return -1;
        }
    }
    s_mz.ok = 1;
    VK_LOG("[aa] depth twins: a stale one gets a copy of its target (depth + %s)\n", st ? "stencil, 8 bit draws" : "no stencil");
    return 0;
}

/* Depth twin <- the target's depth (and stencil), every sample the pixel's
 * value (outside a render pass). 0: the twin holds the picture. */
static int vk_ms_unresolve_depth(vk_eng_obj* o)
{
    const int st = (VK_DS_ASPECT & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    if (!o->zv.view || (st && !o->sv.view) || vk_ms_zcopy_init()) return -1;
    if (!o->ms_set) {
        if (s_mz.sets >= VK_MS_SETS) {
            vk_batch_flush();
            vk_fif_wait_other();
            pvkResetDescriptorPool(s_vk.device, s_mz.pool, 0);
            for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
                if (s_eobj[i].kind == VK_ENG_DEPTH) s_eobj[i].ms_set = VK_NULL_HANDLE;
            s_mz.sets = 0;
        }
        const VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                  .descriptorPool = s_mz.pool, .descriptorSetCount = 1,
                                                  .pSetLayouts = &s_mz.lay };
        if (pvkAllocateDescriptorSets(s_vk.device, &dai, &o->ms_set) != VK_SUCCESS) { o->ms_set = VK_NULL_HANDLE; return -1; }
        s_mz.sets++;
        const VkDescriptorImageInfo ii[2] = { { s_vk.sampler, o->zv.view, VK_IMAGE_LAYOUT_GENERAL },
                                              { s_vk.sampler, st ? o->sv.view : o->zv.view, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet wr[2];
        for (u32 k = 0; k < 2; k++)
            wr[k] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = o->ms_set,
                                            .dstBinding = k, .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[k] };
        pvkUpdateDescriptorSets(s_vk.device, st ? 2u : 1u, wr, 0, NULL);
    }
    if (!o->ms_fb) {
        const VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                              .renderPass = s_mz.rp, .attachmentCount = 1, .pAttachments = &o->ms.view,
                                              .width = o->w, .height = o->h, .layers = 1 };
        if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &o->ms_fb) != VK_SUCCESS) { o->ms_fb = VK_NULL_HANDLE; return -1; }
    }
    if (vk_begin()) return -1;
    vk_barrier_all();
    const VkClearValue cv = { .depthStencil = { 1.0f, 0u } };
    const u32 cw = (o->ms_dw && o->ms_dw < o->w) ? o->ms_dw : o->w, ch = (o->ms_dh && o->ms_dh < o->h) ? o->ms_dh : o->h;
    const VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = s_mz.rp, .framebuffer = o->ms_fb,
                                        .renderArea = { { 0, 0 }, { cw, ch } }, .clearValueCount = 1, .pClearValues = &cv };
    vk_gprof_stamp(GP_BGAP, o->w, o->h);
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp = { 0.0f, 0.0f, (float)o->w, (float)o->h, 0.0f, 1.0f };
    const VkRect2D sc = { { 0, 0 }, { o->w, o->h } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mz.pl, 0, 1, &o->ms_set, 0, NULL);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mz.zpipe);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    if (st)
        for (u32 k = 0; k < 8; k++) {
            pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mz.spipe[k]);
            pvkCmdPushConstants(s_vk.cmd, s_mz.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &k);
            pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
        }
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_gprof_stamp(GP_BPASS, cw, ch);
    s_ecs.valid = 0;
    if (!s_batch.recording && vk_submit_and_wait()) return -1;
    o->ms_ok = vk_ms_full(o); o->ms_dw = o->ms_dh = 0;
    s_mz.copies++;
    return 0;
}

/* Why a pass of eligible targets was drawn 1x (the stats line's order). */
enum { MSW_TARGET = 0, MSW_FBF_IN, MSW_DEPTH, MSW_DEPTH_STALE, MSW_TWIN, MSW_COPY, MSW_RP, MSW_N };
static const char* const kMsWhy[MSW_N] = { "target not eligible", "reads its targets (input attachments)",
                                           "depth not eligible", "depth twin not cleared yet", "no twin",
                                           "colour copy failed", "no render pass" };

/* The draw's pass: s_ms.n (draw into the twins, resolve at the end) or 1
 * (into `im`, as before). Called for every draw before its pass is chosen
 * (outside a render pass when it records: it may end the open one). zo: the
 * depth target the draw names when it fits (NULL: none, the scratch). */
static u32 vk_ms_pass(const vk_eng_pipe* P, u32 n, vk_eng_obj* zo, u32 w, u32 h)
{
    int why = -1, any = zo && zo->ms_elig;
    for (u32 i = 0; i < n; i++) {
        const vk_eng_obj* o = &s_eobj[s_eng_bound.rt[i]];
        any |= o->ms_elig;
        if (why < 0 && (!o->ms_elig || o->w != w || o->h != h)) why = MSW_TARGET;
    }
    if (!any) return 1u;                         /* shadow maps, effects, glow: never multisampled, not counted */
    if (why < 0 && P->fbf_in) why = MSW_FBF_IN;
    if (why < 0 && zo && !zo->ms_elig) why = MSW_DEPTH;
    if (why < 0 && zo && !zo->ms.img && vk_ms_twin(zo)) why = MSW_TWIN;
    if (why < 0 && zo && zo->ms_ok != vk_ms_full(zo) && vk_ms_unresolve_depth(zo)) why = MSW_DEPTH_STALE;
    for (u32 i = 0; i < n && why < 0; i++) {
        vk_eng_obj* o = &s_eobj[s_eng_bound.rt[i]];
        if (!o->ms.img && vk_ms_twin(o)) why = MSW_TWIN;
    }
    if (why < 0 && !vk_ms_render_pass(n)) why = MSW_RP;
    for (u32 i = 0; i < n && why < 0; i++) {
        vk_eng_obj* o = &s_eobj[s_eng_bound.rt[i]];
        if (!o->ms_ok && vk_ms_unresolve(o)) why = MSW_COPY;
    }
    if (why >= 0) {
        s_ms.why[why]++;
        if (s_ms.why[why] == 1)
            VK_LOG("[aa] first 1x pass among multisampled targets: %s (%ux%u, %u colour target(s)%s)\n",
                   kMsWhy[why], w, h, n, zo ? " + depth" : "");
        return 1u;
    }
    return s_ms.n;
}

/* A 1x draw: the twins of what it writes no longer hold the picture. */
static void vk_ms_dirty(vk_eng_obj* o, u32 w, u32 h)
{
    if (w > o->ms_dw) o->ms_dw = (u16)(w < o->w ? w : o->w);
    if (h > o->ms_dh) o->ms_dh = (u16)(h < o->h ? h : o->h);
}
static void vk_ms_note_1x(const vk_eng_pipe* P, u32 n, vk_eng_obj* zo, u32 w, u32 h)
{
    const rsx_be_render_state* rs = &P->rs;
    if (rs->color_mask || rs->color_mask_mrt)
        for (u32 i = 0; i < n; i++) {
            vk_eng_obj* o = &s_eobj[s_eng_bound.rt[i]];
            if (o->ms_ok) { o->ms_ok = 0; o->ms_dw = o->ms_dh = 0; s_ms.stale_c++; }
            if (o->ms.img) vk_ms_dirty(o, w, h);   /* the pass's extent: what it can have written */
        }
    if (zo && zo->ms.img) {
        u8 drop = 0;
        if (rs->depth_test && rs->depth_write) drop |= 1u;
        if (rs->stencil_enable && (rs->s_write_mask & 0xFFu)) drop |= 2u;
        if (drop && zo->ms_ok != vk_ms_full(zo)) vk_ms_dirty(zo, w, h);   /* stale already: the corner grows */
        if (zo->ms_ok & drop) {
            if (zo->ms_ok == vk_ms_full(zo)) zo->ms_dw = zo->ms_dh = 0;
            zo->ms_ok &= (u8)~drop;
            vk_ms_dirty(zo, w, h);
            s_ms.stale_z++;
            static int said;
            if (!said++) VK_LOG("[aa] first 1x draw that writes a multisampled depth target: its twin gets a copy of the written corner before the next MSAA pass\n");
        }
    }
}

/* Every 10 s (the present): what MSAA did. */
static void vk_ms_stats(void)
{
    if (s_ms.n < 2u && !g_eng_aa_progs[0] && !g_eng_aa_progs[2] && !s_zeq.draws) return;
    const u64 now = ps3_ft_now();
    if (!s_ms.t_print) { s_ms.t_print = now; return; }
    if (now - s_ms.t_print < 10000000000ull) return;
    s_ms.t_print = now;
    if (s_ms.n > 1u)
        VK_LOG("[aa] stats: MSAA %ux: passes %llu / 1x %llu, draws %llu / 1x %llu (1x draws of eligible targets "
               "by reason: target %llu, input-att %llu, "
               "depth %llu, depth-stale %llu, twin %llu, copy %llu, rp %llu); twins %llu (%.0f MB), colour copies %llu "
               "(failed %llu), depth copies %llu, stale marks colour %llu depth %llu\n",
               s_ms.n, s_ms.pass_ms, s_ms.pass_1x, s_ms.draw_ms, s_ms.draw_1x,
               s_ms.why[MSW_TARGET], s_ms.why[MSW_FBF_IN], s_ms.why[MSW_DEPTH], s_ms.why[MSW_DEPTH_STALE],
               s_ms.why[MSW_TWIN], s_ms.why[MSW_COPY], s_ms.why[MSW_RP], s_ms.made, s_ms.made_mb,
               s_ms.unres, s_ms.unres_fail, s_mz.copies, s_ms.stale_c, s_ms.stale_z);
    VK_LOG("[aa] stats: alpha-tested programs: thin-cut-out test %llu, other comparison %llu, text mismatch %llu; "
           "mip helpers %llu (no room %llu); LEQUAL re-draws biased %llu\n",
           (unsigned long long)g_eng_aa_progs[0], (unsigned long long)g_eng_aa_progs[1],
           (unsigned long long)g_eng_aa_progs[2], (unsigned long long)g_eng_aa_progs[3],
           (unsigned long long)g_eng_aa_progs[4], s_zeq.draws);
}

/* The engine's shutdown (the device idle, the objects released). */
static void vk_ms_shutdown(void)
{
    if (!s_vk.device || s_ms.n < 2u) return;
    vk_ms_drop_framebuffers();
    vk_batch_flush();
    if (pvkDeviceWaitIdle) pvkDeviceWaitIdle(s_vk.device);
    if (s_ms.scratch.img || s_ms.scratch.view) vk_destroy_image(&s_ms.scratch);
    for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
        if (s_ms.rp[n]) { pvkDestroyRenderPass(s_vk.device, s_ms.rp[n], NULL); s_ms.rp[n] = VK_NULL_HANDLE; }
    if (s_ms.upipe) pvkDestroyPipeline(s_vk.device, s_ms.upipe, NULL);
    if (s_ms.upl)   pvkDestroyPipelineLayout(s_vk.device, s_ms.upl, NULL);
    if (s_ms.upool) pvkDestroyDescriptorPool(s_vk.device, s_ms.upool, NULL);
    if (s_ms.ulay)  pvkDestroyDescriptorSetLayout(s_vk.device, s_ms.ulay, NULL);
    if (s_ms.urp)   pvkDestroyRenderPass(s_vk.device, s_ms.urp, NULL);
    if (s_ms.uvs)   pvkDestroyShaderModule(s_vk.device, s_ms.uvs, NULL);
    if (s_ms.ufs)   pvkDestroyShaderModule(s_vk.device, s_ms.ufs, NULL);
    s_ms.upipe = VK_NULL_HANDLE; s_ms.upl = VK_NULL_HANDLE; s_ms.upool = VK_NULL_HANDLE;
    s_ms.ulay = VK_NULL_HANDLE; s_ms.urp = VK_NULL_HANDLE; s_ms.uvs = s_ms.ufs = VK_NULL_HANDLE;
    s_ms.usets = 0;
    for (u32 k = 0; k < 8; k++) if (s_mz.spipe[k]) pvkDestroyPipeline(s_vk.device, s_mz.spipe[k], NULL);
    if (s_mz.zpipe) pvkDestroyPipeline(s_vk.device, s_mz.zpipe, NULL);
    if (s_mz.pl)    pvkDestroyPipelineLayout(s_vk.device, s_mz.pl, NULL);
    if (s_mz.pool)  pvkDestroyDescriptorPool(s_vk.device, s_mz.pool, NULL);
    if (s_mz.lay)   pvkDestroyDescriptorSetLayout(s_vk.device, s_mz.lay, NULL);
    if (s_mz.rp)    pvkDestroyRenderPass(s_vk.device, s_mz.rp, NULL);
    if (s_mz.zfs)   pvkDestroyShaderModule(s_vk.device, s_mz.zfs, NULL);
    if (s_mz.sfs)   pvkDestroyShaderModule(s_vk.device, s_mz.sfs, NULL);
    memset(&s_mz, 0, sizeof s_mz);
}
