"""Headless RenderDoc capture inspection (runs INSIDE qrenderdoc: `--python`).

    qrenderdoc.exe --python tools\\rdoc_inspect.py      (settings through environment variables)

    RDI_CAPTURE   the .rdc file
    RDI_OUT       output directory (created)
    RDI_MODE      one of
        list      every action: eventId, name, index count, colour/depth outputs, and the
                  render pass grouping -> actions.txt; the final output -> final.png
        pixel     RDI_X, RDI_Y [RDI_TEX = resource id of the target, default: the last colour
                  output]: pixel history of that pixel -> pixel.txt
        event     RDI_EVENTS="eid,eid,...": for each event the full pipeline state (shaders
                  + disassembly, the pixel constants (fp_constants, fp_alpha), every
                  read-only resource with format/size, blend/depth/raster state)
                  -> event_<eid>.txt, and every bound texture and the colour outputs AFTER
                  the event -> event_<eid>_*.png
        targets   RDI_EVENTS: save the colour outputs after each event (quick A/B of passes)
The script exits the process when it is done (no UI window stays open).
First use of qrenderdoc on a machine shows an "Analytics" dialog that blocks --python until
it is answered once (choose and OK; it is then remembered in %APPDATA%\\qrenderdoc).

Typical session (2026-11-08, the Beast bug):
    tools\\combat_beast.ps1 -Tag X -RdocTrigger <file>  (or -RdocAt "440")  -> rdoc_frameN.rdc
    RDI_MODE=list   -> actions.txt (+ passes), final.png
    RDI_MODE=targets RDI_EVENTS=<last draw of each pass>  -> where the picture goes wrong
    RDI_MODE=event  RDI_EVENTS=<that draw>  -> its shader, constants, inputs, blend state
    RDI_MODE=pixel  RDI_TEX=<target id> RDI_X/RDI_Y  -> which draws wrote a broken pixel
"""
import os
import sys
import traceback

import renderdoc as rd

CAP = os.environ.get("RDI_CAPTURE", "")
OUT = os.environ.get("RDI_OUT", ".")
MODE = os.environ.get("RDI_MODE", "list")
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, "rdoc_inspect.log"), "a", encoding="utf-8")


def log(*a):
    LOG.write(" ".join(str(x) for x in a) + "\n")
    LOG.flush()


def walk(actions, depth=0):
    for a in actions:
        yield a, depth
        if a.children:
            yield from walk(a.children, depth + 1)


def tex_desc_map(ctrl):
    return {t.resourceId: t for t in ctrl.GetTextures()}


def res_name_map(ctrl):
    return {r.resourceId: r.name for r in ctrl.GetResources()}


def save_tex(ctrl, rid, path, alpha=False):
    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.destType = rd.FileType.PNG
    ts.mip = 0
    ts.slice.sliceIndex = 0
    ts.alpha = rd.AlphaMapping.Preserve if alpha else rd.AlphaMapping.Discard
    try:
        ctrl.SaveTexture(ts, path)
    except Exception as e:  # noqa
        log("save failed", rid, e)


def fmt_tex(t):
    if t is None:
        return "?"
    return "%dx%dx%d mips=%d arr=%d %s" % (t.width, t.height, t.depth, t.mips, t.arraysize, t.format.Name())


def mode_list(ctrl):
    sf = ctrl.GetStructuredFile()
    texs = tex_desc_map(ctrl)
    names = res_name_map(ctrl)
    last_out = None
    with open(os.path.join(OUT, "actions.txt"), "w", encoding="utf-8") as f:
        for a, d in walk(ctrl.GetRootActions()):
            outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
            name = a.GetName(sf)
            line = "%6d %s%s" % (a.eventId, "  " * d, name)
            if a.flags & rd.ActionFlags.Drawcall:
                line += "  idx=%d inst=%d" % (a.numIndices, a.numInstances)
            if outs:
                line += "  out=" + ",".join("%s(%s)" % (int(o), fmt_tex(texs.get(o))) for o in outs)
                last_out = outs[0]
            if a.depthOut != rd.ResourceId.Null():
                line += "  depth=%s(%s)" % (int(a.depthOut), fmt_tex(texs.get(a.depthOut)))
            f.write(line + "\n")
        f.write("\n# textures\n")
        for rid, t in texs.items():
            f.write("%s %s %s\n" % (int(rid), fmt_tex(t), names.get(rid, "")))
    acts = list(walk(ctrl.GetRootActions()))
    if acts:
        ctrl.SetFrameEvent(acts[-1][0].eventId, True)
    if last_out is not None:
        save_tex(ctrl, last_out, os.path.join(OUT, "final.png"))


def find_rid(ctrl, val):
    for t in ctrl.GetTextures():
        if int(t.resourceId) == int(val):
            return t.resourceId
    return None


def mode_pixel(ctrl):
    x, y = int(os.environ["RDI_X"]), int(os.environ["RDI_Y"])
    rid = None
    if os.environ.get("RDI_TEX"):
        rid = find_rid(ctrl, os.environ["RDI_TEX"])
    if rid is None:
        for a, _ in walk(ctrl.GetRootActions()):
            outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
            if outs:
                rid = outs[0]
    sf = ctrl.GetStructuredFile()
    names = {a.eventId: a.GetName(sf) for a, _ in walk(ctrl.GetRootActions())}
    hist = ctrl.PixelHistory(rid, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    with open(os.path.join(OUT, "pixel_%d_%d.txt" % (x, y)), "w", encoding="utf-8") as f:
        f.write("pixel %d,%d of %s\n" % (x, y, int(rid)))
        for m in hist:
            pre = m.preMod.col.floatValue
            post = m.postMod.col.floatValue
            so = m.shaderOut.col.floatValue
            flags = []
            for nm in ("sampleMasked", "backfaceCulled", "depthClipped", "viewClipped", "scissorClipped",
                       "shaderDiscarded", "depthTestFailed", "stencilTestFailed", "predicationSkipped"):
                if getattr(m, nm, False):
                    flags.append(nm)
            f.write("eid %6d prim %d  pre=(%.4f %.4f %.4f %.4f) shader=(%.4f %.4f %.4f %.4f) post=(%.4f %.4f %.4f %.4f) depth %.6f->%.6f %s  %s\n" % (
                m.eventId, m.primitiveID, pre[0], pre[1], pre[2], pre[3], so[0], so[1], so[2], so[3],
                post[0], post[1], post[2], post[3], m.preMod.depth, m.postMod.depth, " ".join(flags), names.get(m.eventId, "")))


def describe_event(ctrl, eid, f, save_textures=True):
    ctrl.SetFrameEvent(eid, True)
    pipe = ctrl.GetPipelineState()
    texs = tex_desc_map(ctrl)
    names = res_name_map(ctrl)
    f.write("=== event %d\n" % eid)
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
        refl = pipe.GetShaderReflection(stage)
        if refl is None:
            continue
        f.write("--- %s shader %s entry %s\n" % (stage, int(pipe.GetShader(stage)), pipe.GetShaderEntryPoint(stage)))
        try:
            targets = ctrl.GetDisassemblyTargets(True)
            tgt = targets[0]
            for t in targets:
                if "GLSL" in t or "glsl" in t:
                    tgt = t
            f.write("[disassembly: %s]\n" % tgt)
            f.write(ctrl.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, tgt))
            f.write("\n")
        except Exception as e:  # noqa
            f.write("disassembly failed: %s\n" % e)
        try:
            for ci, cb in enumerate(refl.constantBlocks):
                bound = pipe.GetConstantBlock(stage, ci, 0)
                d = bound.descriptor
                vars_ = ctrl.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), refl.resourceId, stage,
                                                        pipe.GetShaderEntryPoint(stage), ci, d.resource,
                                                        d.byteOffset, d.byteSize)

                def dump(vs, pre=""):
                    for v in vs:
                        if v.members:
                            dump(v.members, pre + v.name + ".")
                        else:
                            vals = [v.value.f32v[i] for i in range(max(1, v.columns))]
                            f.write("  cb%d %s%s = %s\n" % (ci, pre, v.name, " ".join("%.6g" % x for x in vals)))
                if stage == rd.ShaderStage.Pixel:
                    dump(vars_)
        except Exception as e:  # noqa
            f.write("cbuffer dump failed: %s\n" % e)
        try:
            used = pipe.GetReadOnlyResources(stage, True)
            for u in used:
                rid = u.descriptor.resource
                acc = u.access
                f.write("  ro bind set=%s bind=%s idx=%s arr=%s -> %s %s %s\n" % (
                    getattr(acc, "descriptorStore", "?"), getattr(acc, "index", "?"),
                    getattr(acc, "arrayElement", "?"), "", int(rid), fmt_tex(texs.get(rid)), names.get(rid, "")))
                if save_textures and rid in texs:
                    save_tex(ctrl, rid, os.path.join(OUT, "event_%d_%s_tex%d.png" % (eid, str(stage).split(".")[-1], int(rid))), alpha=True)
        except Exception as e:  # noqa
            f.write("ro resources failed: %s\n%s\n" % (e, traceback.format_exc()))
        try:
            for u in pipe.GetSamplers(stage, True):
                s = u.sampler
                f.write("  sampler idx=%s addr=%s/%s/%s filter=%s/%s/%s minlod=%s maxlod=%s bias=%s unnorm=%s\n" % (
                    getattr(u.access, "index", "?"), s.addressU, s.addressV, s.addressW,
                    s.filter.minify, s.filter.magnify, s.filter.mip, s.minLOD, s.maxLOD, s.mipBias,
                    getattr(s, "unnormalized", "?")))
        except Exception as e:  # noqa
            f.write("samplers failed: %s\n" % e)
    try:
        vk = ctrl.GetVulkanPipelineState()
        rs = vk.rasterizer
        g = lambda o, n: getattr(o, n, "?")  # noqa: E731 (field names differ between RenderDoc versions)
        f.write("raster: cull=%s frontCCW=%s fill=%s depthClamp=%s bias=%s/%s/%s\n" % (
            g(rs, "cullMode"), g(rs, "frontCCW"), g(rs, "fillMode"), g(rs, "depthClampEnable"),
            g(rs, "depthBias"), g(rs, "slopeScaledDepthBias"), g(rs, "depthBiasClamp")))
        ds = vk.depthStencil
        f.write("depth: test=%s write=%s func=%s stencil=%s\n" % (
            g(ds, "depthTestEnable"), g(ds, "depthWriteEnable"), g(ds, "depthFunction"), g(ds, "stencilTestEnable")))
        for i, b in enumerate(vk.colorBlend.blends):
            cb, ab = g(b, "colorBlend"), g(b, "alphaBlend")
            f.write("blend[%d]: en=%s color src=%s op=%s dst=%s ; alpha src=%s op=%s dst=%s mask=%s\n" % (
                i, g(b, "enabled"), g(cb, "source"), g(cb, "operation"), g(cb, "destination"),
                g(ab, "source"), g(ab, "operation"), g(ab, "destination"), g(b, "writeMask")))
        vp = vk.viewportScissor
        for v in vp.viewportScissors:
            f.write("viewport: %s,%s %sx%s z %s..%s scissor %s,%s %sx%s\n" % (
                v.vp.x, v.vp.y, v.vp.width, v.vp.height, v.vp.minDepth, v.vp.maxDepth,
                v.scissor.x, v.scissor.y, v.scissor.width, v.scissor.height))
        ia = vk.inputAssembly
        f.write("topology=%s\n" % (ia.topology,))
        for a in vk.vertexInput.attributes:
            f.write("  vattr loc=%d binding=%d fmt=%s off=%d\n" % (a.location, a.binding, a.format.Name(), a.byteOffset))
        for b in vk.vertexInput.bindings:
            f.write("  vbind %d stride=%d perInst=%s\n" % (b.vertexBufferBinding, b.byteStride, b.perInstance))
    except Exception as e:  # noqa
        f.write("vk state failed: %s\n%s\n" % (e, traceback.format_exc()))
    for i, o in enumerate(pipe.GetOutputTargets()):
        rid = o.resource
        if rid != rd.ResourceId.Null():
            f.write("output %d: %s %s\n" % (i, int(rid), fmt_tex(texs.get(rid))))
            save_tex(ctrl, rid, os.path.join(OUT, "event_%d_out%d.png" % (eid, i)), alpha=False)
    d = pipe.GetDepthTarget()
    if d.resource != rd.ResourceId.Null():
        f.write("depth target: %s %s\n" % (int(d.resource), fmt_tex(texs.get(d.resource))))


def mode_event(ctrl, save_textures=True):
    eids = [int(x) for x in os.environ.get("RDI_EVENTS", "").split(",") if x.strip()]
    for eid in eids:
        with open(os.path.join(OUT, "event_%d.txt" % eid), "w", encoding="utf-8") as f:
            try:
                describe_event(ctrl, eid, f, save_textures)
            except Exception as e:  # noqa
                f.write("failed: %s\n%s\n" % (e, traceback.format_exc()))


def mode_blendscan(ctrl):
    """Every draw's blend state, one line each -> blends.txt (RDI_FROM/RDI_TO limit the range)."""
    lo = int(os.environ.get("RDI_FROM", "0"))
    hi = int(os.environ.get("RDI_TO", "999999999"))
    with open(os.path.join(OUT, "blends.txt"), "w", encoding="utf-8") as f:
        for a, _ in walk(ctrl.GetRootActions()):
            if not (a.flags & rd.ActionFlags.Drawcall) or a.eventId < lo or a.eventId > hi:
                continue
            ctrl.SetFrameEvent(a.eventId, False)
            vk = ctrl.GetVulkanPipelineState()
            outs = [int(o) for o in a.outputs if o != rd.ResourceId.Null()]
            for i, b in enumerate(vk.colorBlend.blends[:1]):
                f.write("%d out=%s idx=%d en=%s c=%s/%s/%s a=%s/%s/%s mask=%s ps=%s\n" % (
                    a.eventId, outs, a.numIndices, b.enabled,
                    str(b.colorBlend.source).split(".")[-1], str(b.colorBlend.operation).split(".")[-1],
                    str(b.colorBlend.destination).split(".")[-1],
                    str(b.alphaBlend.source).split(".")[-1], str(b.alphaBlend.operation).split(".")[-1],
                    str(b.alphaBlend.destination).split(".")[-1], b.writeMask,
                    int(ctrl.GetPipelineState().GetShader(rd.ShaderStage.Pixel))))


def mode_targets(ctrl):
    eids = [int(x) for x in os.environ.get("RDI_EVENTS", "").split(",") if x.strip()]
    for eid in eids:
        ctrl.SetFrameEvent(eid, True)
        pipe = ctrl.GetPipelineState()
        for i, o in enumerate(pipe.GetOutputTargets()):
            if o.resource != rd.ResourceId.Null():
                save_tex(ctrl, o.resource, os.path.join(OUT, "after_%d_out%d.png" % (eid, i)))


def main():
    log("open", CAP, MODE)
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAP, "", None)
    if res != rd.ResultCode.Succeeded:
        log("OpenFile failed", res)
        return
    if not cap.LocalReplaySupport():
        log("no local replay support")
        return
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        log("OpenCapture failed", res)
        return
    try:
        if MODE == "list":
            mode_list(ctrl)
        elif MODE == "pixel":
            mode_pixel(ctrl)
        elif MODE == "event":
            mode_event(ctrl)
        elif MODE == "targets":
            mode_targets(ctrl)
        elif MODE == "blendscan":
            mode_blendscan(ctrl)
        log("done")
    finally:
        ctrl.Shutdown()
        cap.Shutdown()


try:
    main()
except Exception:  # noqa
    log(traceback.format_exc())
LOG.close()
os._exit(0)
