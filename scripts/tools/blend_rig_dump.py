#!/usr/bin/env python3
"""Read-only dump of an armature rig from a .blend file, without Blender (#1567).

The .blend format is self-describing: a header, then file blocks (code, size, old memory
address, SDNA struct index, count), and a DNA1 block cataloguing every struct (field types and
names). This walks it and prints, for each armature object:

  * bones: name, parent, head / tail (armature space), roll
  * pose channels: constraints (type and every setting of the constraint's data struct:
    Transformation from / to ranges and mapping, Limit Location, Damped Track / IK targets and
    subtargets, Copy Location / Rotation spaces and influence, ...)
  * drivers on the object, its armature, and (with --all-drivers) every ID: FCurve data path,
    array index, driver expression and variables (targets, bone, transform channel, space)

Usage:
  scripts/tools/blend_rig_dump.py FILE.blend [--bones REGEX] [--all-drivers] [--actions REGEX]
                                  [--mesh REGEX [--max-verts N]] [--no-rig]

--bones limits the bone / channel listing to names matching REGEX (case-insensitive); the
constraints of matching channels and any constraint targeting a matching bone are shown.
--actions REGEX: also list the F-Curves (data path, keyframe values) of actions whose name matches.
The file is only read (opened read-only, memory-mapped).
"""
import argparse
import mmap
import re
import struct
import sys

# ---------------------------------------------------------------------------
# File + SDNA
# ---------------------------------------------------------------------------


class Blend:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        head = self.m[:12]
        if head[:7] != b"BLENDER":
            raise SystemExit("not an uncompressed .blend (zstd / gzip files: decompress first)")
        self.psize = 8 if head[7:8] == b"-" else 4
        self.end = "<" if head[8:9] == b"v" else ">"
        self.version = head[9:12].decode()
        self.blocks = []        # (code, offset_of_data, size, addr, sdna, count)
        self.by_addr = {}
        off = 12
        bh = struct.Struct(self.end + "4si" + ("Q" if self.psize == 8 else "I") + "ii")
        while off + bh.size <= len(self.m):
            code, size, addr, sdna, count = bh.unpack_from(self.m, off)
            data = off + bh.size
            code = code.rstrip(b"\0").decode("latin1")
            blk = (code, data, size, addr, sdna, count)
            self.blocks.append(blk)
            if addr:
                self.by_addr[addr] = blk
            if code == "ENDB":
                break
            if code == "DNA1":
                self.parse_dna(data, size)
            off = data + size
        self.addrs_sorted = sorted(self.by_addr)

    def parse_dna(self, off, size):
        m = self.m
        e = self.end
        p = off + 4  # SDNA
        assert m[p:p + 4] == b"NAME"
        p += 4
        (n,) = struct.unpack_from(e + "i", m, p)
        p += 4
        names = []
        for _ in range(n):
            z = m.find(b"\0", p)
            names.append(m[p:z].decode("latin1"))
            p = z + 1
        p = (p + 3) & ~3
        assert m[p:p + 4] == b"TYPE"
        p += 4
        (n,) = struct.unpack_from(e + "i", m, p)
        p += 4
        types = []
        for _ in range(n):
            z = m.find(b"\0", p)
            types.append(m[p:z].decode("latin1"))
            p = z + 1
        p = (p + 3) & ~3
        assert m[p:p + 4] == b"TLEN"
        p += 4
        tlen = list(struct.unpack_from(e + "%dh" % len(types), m, p))
        p += 2 * len(types)
        p = (p + 3) & ~3
        assert m[p:p + 4] == b"STRC"
        p += 4
        (n,) = struct.unpack_from(e + "i", m, p)
        p += 4
        structs = []
        for _ in range(n):
            t, nf = struct.unpack_from(e + "hh", m, p)
            p += 4
            fields = []
            for _ in range(nf):
                ft, fn = struct.unpack_from(e + "hh", m, p)
                p += 4
                fields.append((types[ft], names[fn]))
            structs.append((types[t], fields))
        self.names, self.types, self.tlen = names, types, tlen
        self.structs = structs
        self.struct_index = {name: i for i, (name, _) in enumerate(structs)}
        self.type_len = dict(zip(types, tlen))
        self.layouts = {}

    # Field layout of a struct: name -> (offset, type, is_ptr, array dims, base name)
    def layout(self, sname):
        if sname in self.layouts:
            return self.layouts[sname]
        _, fields = self.structs[self.struct_index[sname]]
        out = {}
        off = 0
        for ftype, fname in fields:
            is_ptr = fname.startswith("*") or fname.startswith("(*")
            dims = [int(d) for d in re.findall(r"\[(\d+)\]", fname)]
            base = re.sub(r"[\*\(\)]|\[\d+\]", "", fname)
            if "(*" in fname:
                base = re.sub(r"\(.*", "", fname.replace("(*", "")).strip()
            count = 1
            for d in dims:
                count *= d
            size = self.psize if is_ptr else self.type_len[ftype]
            out[base] = (off, ftype, is_ptr, dims, fname)
            off += size * count
        self.layouts[sname] = out
        return out

    def find(self, addr):
        """(block, offset in data) holding addr."""
        if not addr:
            return None, 0
        blk = self.by_addr.get(addr)
        if blk:
            return blk, 0
        import bisect
        i = bisect.bisect_right(self.addrs_sorted, addr) - 1
        if i >= 0:
            a = self.addrs_sorted[i]
            blk = self.by_addr[a]
            if addr < a + blk[2]:
                return blk, addr - a
        return None, 0

    def read_prim(self, ftype, off):
        e = self.end
        fmt = {"char": "b", "uchar": "B", "short": "h", "ushort": "H", "int": "i", "uint": "I",
               "float": "f", "double": "d", "int64_t": "q", "uint64_t": "Q", "int8_t": "b",
               "uint8_t": "B", "int16_t": "h", "uint16_t": "H", "int32_t": "i", "uint32_t": "I",
               "long": "i", "ulong": "I"}.get(ftype)
        if fmt is None:
            return None
        return struct.unpack_from(e + fmt, self.m, off)[0]

    def get(self, sname, off, field):
        """One field of struct sname at file offset off (pointers -> address ints, char arrays
        -> str, other arrays -> lists, nested structs -> (sname, offset))."""
        lay = self.layout(sname)
        if field not in lay:
            return None
        foff, ftype, is_ptr, dims, _ = lay[field]
        p = off + foff
        if is_ptr:
            fmt = self.end + ("Q" if self.psize == 8 else "I")
            if dims:
                n = 1
                for d in dims:
                    n *= d
                return [struct.unpack_from(fmt, self.m, p + i * self.psize)[0] for i in range(n)]
            return struct.unpack_from(fmt, self.m, p)[0]
        if ftype == "char" and dims:
            n = 1
            for d in dims:
                n *= d
            raw = self.m[p:p + n]
            z = raw.find(b"\0")
            return (raw if z < 0 else raw[:z]).decode("utf-8", "replace")
        if ftype in self.struct_index:
            if dims:
                n = 1
                for d in dims:
                    n *= d
                return [(ftype, p + i * self.type_len[ftype]) for i in range(n)]
            return (ftype, p)
        if dims:
            n = 1
            for d in dims:
                n *= d
            sz = self.type_len[ftype]
            return [self.read_prim(ftype, p + i * sz) for i in range(n)]
        return self.read_prim(ftype, p)

    def deref(self, addr):
        """(struct name, file offset) of what addr points at (its block's SDNA type)."""
        blk, inner = self.find(addr)
        if not blk:
            return None
        sname = self.structs[blk[4]][0]
        return sname, blk[1] + inner

    def cstring(self, addr):
        blk, inner = self.find(addr)
        if not blk:
            return None
        raw = self.m[blk[1] + inner:blk[1] + blk[2]]
        z = raw.find(b"\0")
        return (raw if z < 0 else raw[:z]).decode("utf-8", "replace")

    def listbase(self, lb):
        """Items of a ListBase (sname, offset) -> [(sname, offset)]."""
        sname, off = lb
        out = []
        addr = self.get(sname, off, "first")
        seen = set()
        while addr and addr not in seen:
            seen.add(addr)
            item = self.deref(addr)
            if not item:
                break
            out.append(item)
            addr = self.get(item[0], item[1], "next")
        return out

    def dump(self, sname, off, skip=("next", "prev"), depth=0):
        """Every plain field of a struct as a dict (nested structs flattened one level)."""
        out = {}
        for name, (foff, ftype, is_ptr, dims, fname) in self.layout(sname).items():
            if name in skip or name.startswith("_pad") or name.startswith("pad"):
                continue
            v = self.get(sname, off, name)
            if is_ptr:
                if not v:
                    continue
                if ftype == "char":
                    v = self.cstring(v)
                elif ftype in ("Object", "ID"):
                    t = self.deref(v)
                    v = self.idname(t) if t else hex(v)
                else:
                    v = "<%s*>" % ftype
            elif isinstance(v, tuple) and depth < 1:
                v = self.dump(v[0], v[1], skip, depth + 1)
            elif isinstance(v, list) and v and isinstance(v[0], tuple):
                v = "<%s[%d]>" % (ftype, len(v))
            if isinstance(v, float):
                v = round(v, 6)
            if isinstance(v, list):
                v = [round(x, 6) if isinstance(x, float) else x for x in v]
            out[name] = v
        return out

    def idname(self, item):
        sname, off = item
        idf = self.get(sname, off, "id")
        if isinstance(idf, tuple):
            return self.get("ID", idf[1], "name")
        return sname

    def blocks_of(self, sname):
        i = self.struct_index.get(sname)
        for blk in self.blocks:
            if blk[4] == i and blk[0] not in ("DNA1", "ENDB"):
                sz = self.type_len[sname]
                for k in range(max(1, blk[5])):
                    yield (sname, blk[1] + k * sz)


# ---------------------------------------------------------------------------
# Rig report
# ---------------------------------------------------------------------------

CONSTRAINT_TYPES = {
    0: "NULL", 1: "CHILDOF", 2: "TRACKTO", 3: "KINEMATIC(IK)", 4: "FOLLOWPATH", 5: "ROTLIMIT",
    6: "LOCLIMIT", 7: "SIZELIMIT", 8: "ROTLIKE", 9: "LOCLIKE", 10: "SIZELIKE", 11: "PYTHON",
    12: "ACTION", 13: "LOCKTRACK", 14: "DISTLIMIT", 15: "STRETCHTO", 16: "MINMAX", 17: "RIGIDBODYJOINT",
    18: "CLAMPTO", 19: "TRANSFORM", 20: "SHRINKWRAP", 21: "DAMPTRACK", 22: "SPLINEIK", 23: "TRANSLIKE",
    24: "SAMEVOL", 25: "PIVOT", 26: "FOLLOWTRACK", 27: "CAMERASOLVER", 28: "OBJECTSOLVER",
    29: "TRANSFORM_CACHE", 30: "ARMATURE",
}
SPACES = {0: "WORLD", 1: "POSE", 2: "LOCAL_WITH_PARENT", 3: "LOCAL", 4: "CUSTOM", 5: "OWNER_LOCAL"}


def bone_tree(b, arm):
    out = []

    def walk(item, parent):
        sname, off = item
        name = b.get(sname, off, "name")
        out.append(dict(name=name, parent=parent,
                        head=[round(x, 5) for x in b.get(sname, off, "arm_head")],
                        tail=[round(x, 5) for x in b.get(sname, off, "arm_tail")],
                        roll=round(b.get(sname, off, "arm_roll") or 0.0, 5),
                        local_head=[round(x, 5) for x in b.get(sname, off, "head")],
                        flag=b.get(sname, off, "flag")))
        for child in b.listbase(b.get(sname, off, "childbase")):
            walk(child, name)

    for root in b.listbase(b.get(arm[0], arm[1], "bonebase")):
        walk(root, None)
    return out


def constraint_report(b, con):
    sname, off = con
    ctype = b.get(sname, off, "type")
    flag = b.get(sname, off, "flag") or 0
    rec = dict(name=b.get(sname, off, "name"), type=CONSTRAINT_TYPES.get(ctype, ctype),
               muted=bool(flag & (1 << 9)),     # CONSTRAINT_OFF
               head_tail=round(b.get(sname, off, "headtail") or 0.0, 4),
               influence=round(b.get(sname, off, "enforce") or 0.0, 4),
               owner_space=SPACES.get(b.get(sname, off, "ownspace"), b.get(sname, off, "ownspace")),
               target_space=SPACES.get(b.get(sname, off, "tarspace"), b.get(sname, off, "tarspace")),
               flag=b.get(sname, off, "flag"))
    data = b.get(sname, off, "data")
    item = b.deref(data) if data else None
    if item:
        rec["data_struct"] = item[0]
        rec["data"] = b.dump(item[0], item[1])
    return rec


def drivers_of(b, owner_name, adt_addr):
    item = b.deref(adt_addr) if adt_addr else None
    if not item:
        return []
    out = []
    for fcu in b.listbase(b.get(item[0], item[1], "drivers")):
        fs, fo = fcu
        path = b.cstring(b.get(fs, fo, "rna_path"))
        idx = b.get(fs, fo, "array_index")
        drv = b.deref(b.get(fs, fo, "driver"))
        rec = dict(owner=owner_name, path=path, index=idx)
        if drv:
            ds, do = drv
            rec["expression"] = b.get(ds, do, "expression")
            rec["driver_type"] = b.get(ds, do, "type")
            vs = []
            for var in b.listbase(b.get(ds, do, "variables")):
                vsn, vo = var
                v = dict(name=b.get(vsn, vo, "name"), type=b.get(vsn, vo, "type"), targets=[])
                ntar = b.get(vsn, vo, "num_targets") or 0
                for t in (b.get(vsn, vo, "targets") or [])[:max(1, ntar)]:
                    ts, to = t
                    idp = b.get(ts, to, "id")
                    tid = b.deref(idp) if idp else None
                    v["targets"].append(dict(id=b.idname(tid) if tid else None,
                                             rna_path=b.cstring(b.get(ts, to, "rna_path")),
                                             bone=b.get(ts, to, "pchan_name"),
                                             transform=b.get(ts, to, "transChan"),
                                             flag=b.get(ts, to, "flag"),
                                             options=b.get(ts, to, "options")))
                vs.append(v)
            rec["variables"] = vs
        # The F-Curve's own keys / modifiers shape the driver's output.
        npts = b.get(fs, fo, "totvert") or 0
        bez = b.deref(b.get(fs, fo, "bezt")) if npts else None
        if bez:
            pts = []
            sz = b.type_len["BezTriple"]
            for k in range(min(npts, 8)):
                vec = b.get("BezTriple", bez[1] + k * sz, "vec")
                pts.append([round(vec[3], 5), round(vec[4], 5)])
            rec["curve_points"] = pts
        mods = b.listbase(b.get(fs, fo, "modifiers"))
        if mods:
            rec["modifiers"] = []
            for md in mods:
                ms, mo = md
                mdata = b.deref(b.get(ms, mo, "data"))
                rec["modifiers"].append(dict(type=b.get(ms, mo, "type"),
                                             data=b.dump(mdata[0], mdata[1]) if mdata else None))
        out.append(rec)
    return out


def action_report(b, regex):
    rx = re.compile(regex, re.I)
    for act in b.blocks_of("bAction"):
        name = b.idname(act)
        if not rx.search(name or ""):
            continue
        print("\n## action %s" % name)
        for fcu in b.listbase(b.get(act[0], act[1], "curves")):
            fs, fo = fcu
            path = b.cstring(b.get(fs, fo, "rna_path"))
            idx = b.get(fs, fo, "array_index")
            npts = b.get(fs, fo, "totvert") or 0
            bez = b.deref(b.get(fs, fo, "bezt")) if npts else None
            vals = []
            if bez:
                sz = b.type_len["BezTriple"]
                for k in range(npts):
                    vec = b.get("BezTriple", bez[1] + k * sz, "vec")
                    vals.append((round(vec[3], 2), round(vec[4], 5)))
            print("  %s[%d]: %s" % (path, idx, vals[:12] if len(vals) > 12 else vals))


def mesh_report(b, regex, max_verts):
    """Objects whose name matches: transform, parent (bone), and the mesh's vertex positions
    (the "position" attribute of Blender 3.5+, else the legacy MVert array), with a
    sphere fit (centre, radius) of the vertices in world space."""
    rx = re.compile(regex, re.I)
    for obj in b.blocks_of("Object"):
        oname = b.idname(obj)
        if not rx.search(oname or ""):
            continue
        os_, oo = obj
        mat = b.get(os_, oo, "object_to_world") or b.get(os_, oo, "obmat")
        mat = [[mat[r * 4 + c] for c in range(4)] for r in range(4)]   # rows = axes, row 3 = translation
        par = b.get(os_, oo, "parent")
        par_item = b.deref(par) if par else None
        print("\n## object %s parent=%s parsubstr=%s partype=%s" % (
            oname, b.idname(par_item) if par_item else None, b.get(os_, oo, "parsubstr"), b.get(os_, oo, "partype")))
        print("  world matrix (column-major rows): %s" % [[round(x, 5) for x in r] for r in mat])
        print("  loc=%s rot=%s scale=%s" % ([round(x, 5) for x in b.get(os_, oo, "loc")],
                                             [round(x, 5) for x in b.get(os_, oo, "rot")],
                                             [round(x, 5) for x in (b.get(os_, oo, "scale") or b.get(os_, oo, "size") or [])]))
        data = b.get(os_, oo, "data")
        me = b.deref(data) if data else None
        if not me or me[0] != "Mesh":
            continue
        ms, mo = me
        nverts = b.get(ms, mo, "totvert")
        verts = []
        vdata = b.get(ms, mo, "vdata")
        layers_addr = b.get(vdata[0], vdata[1], "layers")
        nlayers = b.get(vdata[0], vdata[1], "totlayer") or 0
        lay = b.deref(layers_addr) if layers_addr else None
        if lay:
            sz = b.type_len["CustomDataLayer"]
            for k in range(nlayers):
                lo = lay[1] + k * sz
                name = b.get("CustomDataLayer", lo, "name")
                if name == "position":
                    d = b.find(b.get("CustomDataLayer", lo, "data"))
                    if d[0]:
                        base = d[0][1] + d[1]
                        for v in range(nverts):
                            verts.append(struct.unpack_from(b.end + "3f", b.m, base + 12 * v))
        if not verts:
            mv = b.deref(b.get(ms, mo, "mvert")) if "mvert" in b.layout("Mesh") else None
            if mv:
                sz = b.type_len["MVert"]
                for v in range(nverts):
                    verts.append(tuple(b.get("MVert", mv[1] + v * sz, "co")))
        print("  mesh %s: %d vertices" % (b.idname(me), len(verts)))
        world = []
        for v in verts:
            world.append(tuple(sum(v[i] * mat[i][j] for i in range(3)) + mat[3][j] for j in range(3)))
        for i, w in enumerate(world[:max_verts]):
            print("    v%-4d local=%s world=%s" % (i, [round(x, 5) for x in verts[i]], [round(x, 5) for x in w]))
        if len(world) >= 4:
            xs = [w[0] for w in world]
            ys = [w[1] for w in world]
            zs = [w[2] for w in world]
            print("  world bbox x %.5f..%.5f y %.5f..%.5f z %.5f..%.5f" % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
            for side, pick in (("x>0", lambda w: w[0] > 0), ("x<0", lambda w: w[0] < 0), ("all", lambda w: True)):
                pts = [w for w in world if pick(w)]
                if len(pts) >= 4:
                    c, r, rms = sphere_fit(pts)
                    print("  sphere fit %s (%d pts): centre=%s radius=%.5f rms=%.5f" % (
                        side, len(pts), [round(x, 5) for x in c], r, rms))


def sphere_fit(pts):
    """Least-squares sphere: |p|^2 = 2 c.p + (r^2 - |c|^2)."""
    import numpy as np
    a = np.array([[2 * p[0], 2 * p[1], 2 * p[2], 1.0] for p in pts])
    y = np.array([p[0] ** 2 + p[1] ** 2 + p[2] ** 2 for p in pts])
    sol, *_ = np.linalg.lstsq(a, y, rcond=None)
    c = sol[:3]
    r = float(np.sqrt(sol[3] + c.dot(c)))
    d = np.sqrt(((np.array(pts) - c) ** 2).sum(axis=1)) - r
    return c.tolist(), r, float(np.sqrt((d ** 2).mean()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("blend")
    ap.add_argument("--bones", default="", help="regex: bones / channels to show (default all)")
    ap.add_argument("--all-drivers", action="store_true", help="drivers of every ID, not just armatures")
    ap.add_argument("--actions", default=None, help="regex: list matching actions' F-Curves")
    ap.add_argument("--mesh", default=None, help="regex: objects to dump (transform, parent, vertices, sphere fit)")
    ap.add_argument("--max-verts", type=int, default=12, help="vertices printed per mesh (--mesh)")
    ap.add_argument("--no-rig", action="store_true", help="skip the armature listing")
    args = ap.parse_args()
    b = Blend(args.blend)
    rx = re.compile(args.bones, re.I) if args.bones else None
    print("# %s: Blender %s, %d-bit, %d blocks" % (args.blend, b.version, b.psize * 8, len(b.blocks)))

    for obj in ([] if args.no_rig else b.blocks_of("Object")):
        os_, oo = obj
        data = b.get(os_, oo, "data")
        target = b.deref(data) if data else None
        if not target or target[0] != "bArmature":
            continue
        oname = b.idname(obj)
        print("\n## armature object %s (data %s)" % (oname, b.idname(target)))
        bones = bone_tree(b, target)
        print("### bones (%d)" % len(bones))
        for bone in bones:
            if rx and not rx.search(bone["name"]) and not (bone["parent"] and rx.search(bone["parent"])):
                continue
            print("  %-28s parent=%-24s head=%s tail=%s roll=%s local_head=%s" % (
                bone["name"], bone["parent"], bone["head"], bone["tail"], bone["roll"], bone["local_head"]))
        pose = b.get(os_, oo, "pose")
        pose_item = b.deref(pose) if pose else None
        if pose_item:
            print("### pose channels with constraints")
            for ch in b.listbase(b.get(pose_item[0], pose_item[1], "chanbase")):
                cs, co = ch
                name = b.get(cs, co, "name")
                cons = [constraint_report(b, c) for c in b.listbase(b.get(cs, co, "constraints"))]
                hit = (not rx) or rx.search(name) or any(
                    rx.search(str(c.get("data", {}).get("subtarget", ""))) for c in cons)
                if not hit:
                    continue
                loc = [round(x, 5) for x in b.get(cs, co, "loc")]
                limits = dict(limitmin=b.get(cs, co, "limitmin"), limitmax=b.get(cs, co, "limitmax"),
                              protectflag=b.get(cs, co, "protectflag"), ikflag=b.get(cs, co, "ikflag"))
                if not cons and rx is None:
                    continue
                print("  [%s] loc=%s rotmode=%s %s" % (name, loc, b.get(cs, co, "rotmode"), limits))
                for c in cons:
                    print("    - %s" % c)
        for rec in drivers_of(b, oname, b.get(os_, oo, "adt")):
            if rx and not rx.search(str(rec)):
                continue
            print("  driver %s" % rec)
        for rec in drivers_of(b, b.idname(target), b.get(target[0], target[1], "adt")):
            if rx and not rx.search(str(rec)):
                continue
            print("  driver %s" % rec)

    if args.all_drivers:
        print("\n## drivers on other IDs")
        for sname in ("Key", "Mesh", "Object", "Material", "bArmature", "Scene"):
            if sname not in b.struct_index:
                continue
            for item in b.blocks_of(sname):
                adt = b.get(item[0], item[1], "adt")
                if not adt:
                    continue
                for rec in drivers_of(b, b.idname(item), adt):
                    if rx and not rx.search(str(rec)):
                        continue
                    print("  driver %s" % rec)
    if args.actions:
        action_report(b, args.actions)
    if args.mesh:
        mesh_report(b, args.mesh, args.max_verts)


if __name__ == "__main__":
    main()
