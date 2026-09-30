#!/usr/bin/env python3
"""Project Bean characters and heads -> model pack OBJs for the GE-X files they replace.

GE-X keeps GoldenEye's N64 geometry and skeleton, whose rest pose is a star
(arms out, legs splayed sideways); Bean's bind stands. Each Bean bone is
rotated so its segment lies along the GE-X skeleton's, the whole is scaled
by the ratio of the two skeletons' limb lengths, and every triangle goes to
the list nodes of its dominant bone - near and far LOD alike, since the game
draws one of the two.

Every Bean body carries a head. What happens to it depends on the body:
  integrated  GoldenEye drew this character's head as part of the body and
              GE-X moved it into a head file of its own (Natalya, Xenia...):
              the NECK bone's triangles go to that head file, in the head's
              own space (origin at the neck joint, which is the body's head
              spot), and the body's neck stub is given a degenerate triangle
              so its N64 geometry does not draw under the new head.
  generic     a guard, a Bond, a civilian: GE-X grafts one of many heads on
              it, so Bean's built-in head is dropped and the body's neck
              nodes are left without a group - an absent group keeps the N64
              neck stub - and whatever head GE-X picks draws on top.
A Bean head file (new/head/) converts on its own: its neck-skinned triangles
in the head's space, at the scale the bodies measured.

The OBJs are in the n64/ convention (model-packs.md): one group per list node
named nodeN, in model space with the node's rest offset added. A head file's
offset is zero (it has no position nodes; xblaMeshNodeOwnRestOffset()).

    bean2pack.py BEAN_CHAR GEX_BODY GEX_HEAD PACK_N64_DIR BODYNAME HEADNAME
"""
import math, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bean2obj import Model, skin_bone
from pdmodel import PdModel, load

CHILD = {'SKEL_BASE': 'SKEL_BACK', 'SKEL_BACK': 'SKEL_NECK'}
for _side in ('LF', 'RT'):
    CHILD['SKEL_%s_SHOULDER' % _side] = 'SKEL_%s_ELBOW' % _side
    CHILD['SKEL_%s_ELBOW' % _side] = 'SKEL_%s_WRIST' % _side
    CHILD['SKEL_%s_HIP' % _side] = 'SKEL_%s_KNEE' % _side
    CHILD['SKEL_%s_KNEE' % _side] = 'SKEL_%s_ANKLE' % _side
INHERIT = {'SKEL_NECK': 'SKEL_BACK', 'SKEL_POSITION': 'SKEL_BASE'}
for _side in ('LF', 'RT'):
    INHERIT['SKEL_%s_WRIST' % _side] = 'SKEL_%s_ELBOW' % _side
    INHERIT['SKEL_%s_ANKLE' % _side] = 'SKEL_%s_KNEE' % _side


def classify_parts(body):
    """part number -> SKEL name, from the list nodes' rest offsets."""
    parts = {}
    for ln in body.listnodes:
        parts.setdefault(ln['part'], ln['rest'])
    names, limbs = {}, {}
    for part, (x, y, z) in parts.items():
        if part is None:
            names[part] = 'SKEL_BASE'
        elif abs(x) < 30:
            names[part] = 'SKEL_BACK' if y < 200 else 'SKEL_NECK'
        else:
            side = 'LF' if x > 0 else 'RT'
            limb = 'arm' if y > 200 else 'leg'
            limbs.setdefault((side, limb), []).append((abs(x), part))
    for (side, limb), lst in limbs.items():
        order = ['SHOULDER', 'ELBOW', 'WRIST'] if limb == 'arm' else ['HIP', 'KNEE', 'ANKLE']
        if len(lst) != 3:
            raise ValueError('%s %s has %d joints' % (side, limb, len(lst)))
        for (_, part), joint in zip(sorted(lst), order):
            names[part] = 'SKEL_%s_%s' % (side, joint)
    return names, parts


def rotation_between(a, b):
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else -np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1 / (1 + c))


def write_textures(bean, outdir, tag):
    """The model's pictures as bean_<tag>_N.png, written once however many files use them."""
    from PIL import Image
    names = []
    for i, (tn, w, h, fmt, rgba) in enumerate(bean.textures):
        fn = 'bean_%s_%d.png' % (tag, i)
        path = os.path.join(outdir, fn)
        if rgba is not None and not os.path.exists(path):
            Image.fromarray(rgba, 'RGBA').save(path)
        names.append(fn)
    return names


def write_obj(outdir, name, texfiles, o):
    with open(os.path.join(outdir, name + '.mtl'), 'w') as f:
        for i, fn in enumerate(texfiles):
            f.write('newmtl bean%d\nKd 1 1 1\nmap_Kd %s\n\n' % (i, fn))
    lines = ['mtllib %s.mtl' % name]
    lines += ['v %.4f %.4f %.4f' % tuple(p) for p in o['v']]
    lines += ['vt %.6f %.6f' % t for t in o['vt']]
    lines += ['vn %.4f %.4f %.4f' % tuple(n) for n in o['vn']]
    ntris = 0
    for k in sorted(o['groups']):
        lines.append('g node%d' % k)
        cur = None
        for tex, idx in o['groups'][k]:
            if tex != cur:
                lines.append('usemtl bean%d' % tex)
                cur = tex
            lines.append('f ' + ' '.join('%d/%d/%d' % (i, i, i) for i in idx))
            ntris += 1
    with open(os.path.join(outdir, name + '.obj'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return ntris


def new_obj():
    return dict(v=[], vt=[], vn=[], groups={})


def head_groups(head, tris):
    """A head file's groups: the head on its untoggled list nodes, nothing on the toggled ones.

    A head is a near/far pair for the face and hair, and on most of GE-X's
    heads two toggled pieces with pairs of their own (glasses, a hat, a
    second hair). Bean's head already carries all of it, so a toggled node
    takes a degenerate triangle - it is not absent, which would keep its N64
    piece drawing over the new head.
    """
    groups = {}
    for ln in head.listnodes:
        if 0x12 in ln['ancestors']:
            groups[ln['index']] = [(0, [1, 1, 1])]
        else:
            groups[ln['index']] = tris
    return groups


def add_vertex(o, pos, uv, nrm):
    o['v'].append(pos)
    o['vt'].append(uv)
    o['vn'].append(nrm)
    return len(o['v'])


def triangle_bones(bean, d, verts, tri):
    """Per vertex [(bone name, weight)], and the triangle's dominant bone."""
    owner, vbones = {}, []
    for i in tri:
        v = verts[i]
        bw = []
        for s in range(4):
            if v['bones'][s] is None or v['weights'][s] == 0 or v['bones'][s] >= len(d['pal']):
                continue
            name = bean.pose[skin_bone(bean, d, v['bones'][s])]['name']
            bw.append((name, v['weights'][s]))
            owner[name] = owner.get(name, 0) + v['weights'][s]
        vbones.append(bw)
    return vbones, (max(owner, key=owner.get) if owner else None)


def convert_body(beanpath, bodypath, bodyname, outdir, tag, heads=(), log=print):
    """heads: [(head file path, head name)] for an integrated head; empty for a generic body."""
    bean = Model(beanpath)
    body = PdModel(load(bodypath))
    partname, partrest = classify_parts(body)
    joint = {partname[p]: np.array(r) for p, r in partrest.items()}
    joint['SKEL_POSITION'] = joint['SKEL_BASE']
    bind = {b['name']: np.array(b['bind']) for b in bean.pose}

    num = den = 0.0
    for a, b in CHILD.items():
        if a != 'SKEL_BASE':
            num += np.linalg.norm(joint[b] - joint[a])
            den += np.linalg.norm(bind[b] - bind[a])
    scale = num / den
    rot = {a: rotation_between(bind[b] - bind[a], joint[b] - joint[a]) for a, b in CHILD.items()}
    for a, b in INHERIT.items():
        rot[a] = rot[b]

    texfiles = write_textures(bean, outdir, tag)
    nodes_for = {}
    for ln in body.listnodes:
        nodes_for.setdefault(partname[ln['part']], []).append(ln['index'])
    nodes_for['SKEL_POSITION'] = nodes_for['SKEL_BASE']

    headobjs = [(PdModel(load(hp)), hn) for hp, hn in heads]
    ob, oh = new_obj(), new_obj()
    cache = {}
    dropped = 0
    for d in bean.draws:
        if d['vb'] not in cache:
            cache[d['vb']] = bean.vertices(d['vb'])
        verts = cache[d['vb']]
        mapped = {}
        for tri in bean.triangles(d):
            vbones, bone = triangle_bones(bean, d, verts, tri)
            if bone is None:
                continue
            if bone == 'SKEL_NECK' and not headobjs:
                dropped += 1
                continue
            tohead = bone == 'SKEL_NECK'
            o = oh if tohead else ob
            idx = []
            for i, bw in zip(tri, vbones):
                key = (tohead, i)
                if key not in mapped:
                    v = verts[i]
                    p = np.array(v['pos'])
                    if tohead:
                        posed = scale * rot['SKEL_NECK'] @ (p - bind['SKEL_NECK'])
                        nrm = rot['SKEL_NECK'] @ np.array(v['nrm'])
                    else:
                        total = sum(w for _, w in bw)
                        posed = sum(w * (joint[n] + scale * rot[n] @ (p - bind[n])) for n, w in bw) / total
                        nrm = rot[max(bw, key=lambda t: t[1])[0]] @ np.array(v['nrm'])
                    mapped[key] = add_vertex(o, posed, (v['uv'][0], 1 - v['uv'][1]), nrm)
                idx.append(mapped[key])
            if tohead:
                oh['groups'].setdefault(0, []).append((d['tex'], idx))
            else:
                for k in nodes_for.get(bone, nodes_for['SKEL_BASE']):
                    ob['groups'].setdefault(k, []).append((d['tex'], idx))

    if headobjs and ob['v']:
        for k in nodes_for.get('SKEL_NECK', []):
            ob['groups'].setdefault(k, []).append((0, [1, 1, 1]))
    nb = write_obj(outdir, bodyname, texfiles, ob)
    log('  %-20s <- %-12s scale %.4f  %5d tris, %d groups%s' % (
        bodyname, tag, scale, nb, len(ob['groups']),
        ', head dropped (%d tris)' % dropped if not headobjs else ''))
    for head, hn in headobjs:
        oo = dict(v=oh['v'], vt=oh['vt'], vn=oh['vn'], groups=head_groups(head, oh['groups'].get(0, [])))
        nh = write_obj(outdir, hn, texfiles, oo)
        log('  %-20s <- %-12s head, %5d tris over %d nodes' % (hn, tag, nh, len(head.listnodes)))
    return scale


def convert_head(beanpath, headpath, headname, outdir, tag, scale, log=print):
    """A Bean head file: its neck-skinned triangles, rigid on the neck, in the head's own space."""
    bean = Model(beanpath)
    head = PdModel(load(headpath))
    bind = {b['name']: np.array(b['bind']) for b in bean.pose}
    texfiles = write_textures(bean, outdir, tag)
    o = new_obj()
    tris = []
    cache = {}
    skipped = 0
    for d in bean.draws:
        if d['vb'] not in cache:
            cache[d['vb']] = bean.vertices(d['vb'])
        verts = cache[d['vb']]
        mapped = {}
        for tri in bean.triangles(d):
            _, bone = triangle_bones(bean, d, verts, tri)
            if bone != 'SKEL_NECK':
                skipped += 1
                continue
            idx = []
            for i in tri:
                if i not in mapped:
                    v = verts[i]
                    posed = scale * (np.array(v['pos']) - bind['SKEL_NECK'])
                    mapped[i] = add_vertex(o, posed, (v['uv'][0], 1 - v['uv'][1]), np.array(v['nrm']))
                idx.append(mapped[i])
            tris.append((d['tex'], idx))
    o['groups'] = head_groups(head, tris)
    n = write_obj(outdir, headname, texfiles, o)
    log('  %-20s <- %-12s head, %5d tris over %d nodes (%d not on the neck, left out)' % (
        headname, tag, n, len(head.listnodes), skipped))


if __name__ == '__main__':
    beanpath, bodypath, headpath, outdir, bodyname, headname = sys.argv[1:7]
    os.makedirs(outdir, exist_ok=True)
    convert_body(beanpath, bodypath, bodyname, outdir,
                 os.path.basename(os.path.dirname(beanpath)), heads=[(headpath, headname)])
