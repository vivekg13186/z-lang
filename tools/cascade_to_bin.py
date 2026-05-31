#!/usr/bin/env python3
"""
cascade_to_bin.py — convert an OpenCV Haar cascade XML file to z's compact
binary format. Run once at build time; the resulting `.zhc` file is read by
z_cv.h at runtime.

Handles BOTH cascade XML layouts:
  - Legacy (OpenCV ≤ 1.x): inline features under each tree, <size> for window
  - Modern (OpenCV 2.x+):   separate <features> array, stages reference them
                            by index via <weakClassifiers>/<internalNodes>,
                            window dimensions in <width>/<height>

Binary layout (all little-endian):

  uint32   magic                'Z' 'H' 'C' '1'
  uint32   window_w
  uint32   window_h
  uint32   n_stages
  for each stage:
      float32  stage_threshold
      uint32   n_trees
      for each tree:
          uint32 n_features
          for each feature:
              float32  threshold
              float32  left_val
              float32  right_val
              uint32   n_rects        (always 2 or 3 in standard cascades)
              for each rect:
                  int32  x
                  int32  y
                  int32  w
                  int32  h
                  float32 weight

Tilted features are not supported and abort with a clear error — all
face cascades shipped with OpenCV are non-tilted.

Usage:
    python3 tools/cascade_to_bin.py haarcascade_frontalface_default.xml face.zhc
"""
import sys, struct, xml.etree.ElementTree as ET

MAGIC = b"ZHC1"


def parse_modern(cascade_root, out):
    """Modern format: stages reference features by index."""
    w = int(cascade_root.findtext("width"))
    h = int(cascade_root.findtext("height"))
    feat_type = cascade_root.findtext("featureType")
    if feat_type and feat_type.strip() != "HAAR":
        sys.stderr.write(f"unsupported featureType '{feat_type}' (need HAAR)\n")
        return None

    # Read the features table once.
    feats_node = cascade_root.find("features")
    if feats_node is None:
        sys.stderr.write("no <features> element in modern cascade\n")
        return None
    features = []
    for f in feats_node:
        rects = list(f.find("rects"))
        tilted = (f.findtext("tilted") or "0").strip()
        if tilted not in ("0", "0."):
            sys.stderr.write("tilted features not supported\n")
            return None
        rs = []
        for r in rects:
            vals = r.text.split()
            x, y, ww, hh = (int(v) for v in vals[:4])
            weight = float(vals[4])
            rs.append((x, y, ww, hh, weight))
        features.append(rs)

    stages_node = cascade_root.find("stages")
    stages = list(stages_node)
    out += struct.pack("<III", w, h, len(stages))

    feature_count = 0
    for s in stages:
        st_thr = float(s.findtext("stageThreshold"))
        weaks = list(s.find("weakClassifiers"))
        out += struct.pack("<fI", st_thr, len(weaks))
        for wc in weaks:
            internal = wc.findtext("internalNodes").split()
            leaves   = wc.findtext("leafValues").split()
            # Stumps: one internal node, two leaf values.
            # internalNodes layout: <node_left> <node_right> <feature_idx> <threshold>
            # When non-stump trees are present, layout is more complex but
            # cascades shipped with OpenCV are all stumps in practice.
            feat_idx = int(internal[2])
            thr      = float(internal[3])
            left_v   = float(leaves[0])
            right_v  = float(leaves[1])
            rects = features[feat_idx]
            # One-feature tree wrapping this single weak-classifier feature.
            out += struct.pack("<I", 1)                           # n_features
            out += struct.pack("<fffI", thr, left_v, right_v, len(rects))
            for rx, ry, rw, rh, rweight in rects:
                out += struct.pack("<iiiif", rx, ry, rw, rh, rweight)
            feature_count += 1
    return feature_count


def parse_legacy(cascade_root, out):
    """Legacy format: inline features under each tree under each stage."""
    size = cascade_root.findtext("size")
    if not size:
        return None
    win_w, win_h = (int(x) for x in size.split())
    stages = list(cascade_root.find("stages"))
    out += struct.pack("<III", win_w, win_h, len(stages))

    feature_count = 0
    for s in stages:
        st_thr = float(s.findtext("stage_threshold"))
        trees = list(s.find("trees"))
        out += struct.pack("<fI", st_thr, len(trees))
        for t in trees:
            features = list(t)
            out += struct.pack("<I", len(features))
            for f in features:
                feat = f.find("feature")
                rects = list(feat.find("rects"))
                tilted = (feat.findtext("tilted") or "0").strip()
                if tilted not in ("0", "0."):
                    sys.stderr.write("tilted features not supported\n")
                    return None
                thr = float(f.findtext("threshold"))
                lv  = float(f.findtext("left_val"))
                rv  = float(f.findtext("right_val"))
                out += struct.pack("<fffI", thr, lv, rv, len(rects))
                for r in rects:
                    vals = r.text.split()
                    x, y, w, h = (int(v) for v in vals[:4])
                    weight = float(vals[4])
                    out += struct.pack("<iiiif", x, y, w, h, weight)
                feature_count += 1
    return feature_count


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: cascade_to_bin.py cascade.xml out.zhc\n")
        return 1
    in_path, out_path = argv[1], argv[2]

    tree = ET.parse(in_path)
    root = tree.getroot()
    if root.tag != "opencv_storage":
        sys.stderr.write(f"unexpected root tag '{root.tag}'\n")
        return 1
    cascade_root = next(iter(root))    # first child holds the cascade

    out = bytearray()
    out += MAGIC

    # Detect layout: modern cascades have <width>/<height>/<features>,
    # legacy ones have <size> and inline features.
    if cascade_root.find("features") is not None:
        n = parse_modern(cascade_root, out)
        fmt = "modern"
    elif cascade_root.findtext("size"):
        n = parse_legacy(cascade_root, out)
        fmt = "legacy"
    else:
        sys.stderr.write(
            "couldn't recognise cascade layout — expected either <size> "
            "(legacy) or <width>/<height>+<features> (modern)\n")
        return 1
    if n is None:
        return 1

    with open(out_path, "wb") as fh:
        fh.write(out)
    sys.stderr.write(
        f"wrote {out_path}: {fmt} format, {n} features, {len(out)} bytes\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
