#!/usr/bin/env python3
"""Live webcam -> dsco avatar parameter stream.

This intentionally does two separate things:

1. stdout stays small and real-time: one JSON avatar parameter object per frame.
2. optional dense landmark logging writes all raw MediaPipe landmarks plus a
   deterministic 10k-point sampled surface to JSONL for calibration/debugging.

Run with:
  python3 face_capture.py --preview --landmark-file /tmp/dsco_face_landmarks.jsonl \
    | ./dsco --anim '{"kind":"avatar","stdin_params":true,"view":"face"}'
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path
from typing import Iterable

try:
    import cv2
    import mediapipe as mp
    import numpy as np
    from mediapipe.tasks import python as mp_tasks
    from mediapipe.tasks.python import vision as mp_vision
except ModuleNotFoundError as exc:
    print(
        "face_capture: missing Python dependency "
        f"{exc.name!r}; install opencv-contrib-python, mediapipe, and numpy, "
        "or set DSCO_FACE_PYTHON to a prepared interpreter",
        file=sys.stderr,
    )
    raise SystemExit(2) from exc


def clamp(x: float, lo: float, hi: float) -> float:
    return lo if x < lo else hi if x > hi else x


def p2(pts: np.ndarray, idx: int) -> np.ndarray:
    if idx >= len(pts):
        return pts[-1, :2]
    return pts[idx, :2]


def dist2(a: np.ndarray, b: np.ndarray) -> float:
    d = a[:2] - b[:2]
    return float(math.hypot(float(d[0]), float(d[1])))


def edge_pair(edge: object) -> tuple[int, int]:
    if hasattr(edge, "start") and hasattr(edge, "end"):
        return int(edge.start), int(edge.end)
    a, b = edge  # type: ignore[misc]
    return int(a), int(b)


def build_triangles(edges: Iterable[object]) -> list[tuple[int, int, int]]:
    graph: dict[int, set[int]] = {}
    for edge in edges:
        a, b = edge_pair(edge)
        if a == b:
            continue
        graph.setdefault(a, set()).add(b)
        graph.setdefault(b, set()).add(a)

    triangles: set[tuple[int, int, int]] = set()
    for a, nbrs in graph.items():
        for b in nbrs:
            if b <= a:
                continue
            for c in nbrs.intersection(graph.get(b, set())):
                if c > b:
                    triangles.add((a, b, c))
    return sorted(triangles)


def dense_landmarks(pts: np.ndarray, triangles: list[tuple[int, int, int]], target: int) -> np.ndarray:
    """Return exactly target points: raw landmarks first, then surface samples."""
    if target <= 0:
        return np.zeros((0, 3), dtype=np.float32)

    raw = pts.astype(np.float32)
    if len(raw) >= target:
        return raw[:target]

    valid = [t for t in triangles if max(t) < len(raw)]
    if not valid:
        reps = int(math.ceil(target / max(len(raw), 1)))
        return np.tile(raw, (reps, 1))[:target]

    tri_pts = np.array([[raw[a], raw[b], raw[c]] for a, b, c in valid], dtype=np.float32)
    cross = np.cross(tri_pts[:, 1] - tri_pts[:, 0], tri_pts[:, 2] - tri_pts[:, 0])
    areas = np.linalg.norm(cross, axis=1)
    if float(areas.sum()) <= 1e-12:
        areas[:] = 1.0

    need = target - len(raw)
    weights = areas / areas.sum() * need
    counts = np.floor(weights).astype(int)
    missing = need - int(counts.sum())
    if missing > 0:
        order = np.argsort(-(weights - counts))
        counts[order[:missing]] += 1

    samples: list[np.ndarray] = [raw]
    phi_a = 0.7548776662466927
    phi_b = 0.5698402909980532
    for tri, count in zip(tri_pts, counts):
        if count <= 0:
            continue
        a, b, c = tri
        rows = np.empty((count, 3), dtype=np.float32)
        for i in range(count):
            u = (i * phi_a) % 1.0
            v = (i * phi_b) % 1.0
            if u + v > 1.0:
                u = 1.0 - u
                v = 1.0 - v
            rows[i] = a + u * (b - a) + v * (c - a)
        samples.append(rows)
    return np.vstack(samples)[:target]


class NeutralCalibration:
    def __init__(self, frames: int) -> None:
        self.frames = max(0, frames)
        self.count = 0
        self.sums: dict[str, float] = {}

    @property
    def ready(self) -> bool:
        return self.count >= self.frames

    def update(self, metrics: dict[str, float]) -> None:
        if self.ready:
            return
        self.count += 1
        for key, value in metrics.items():
            self.sums[key] = self.sums.get(key, 0.0) + value

    def value(self, key: str, fallback: float) -> float:
        if self.count <= 0:
            return fallback
        return self.sums.get(key, fallback * self.count) / self.count

    def to_json(self) -> dict[str, float | int]:
        out: dict[str, float | int] = {"frames": self.count}
        for key, value in self.sums.items():
            out[key] = value / max(self.count, 1)
        return out


def extract_avatar_params(pts: np.ndarray, name: str, calib: NeutralCalibration) -> tuple[dict[str, float | int | str], dict[str, float]]:
    # FaceMesh with refine_landmarks may return iris landmarks after 468; use all
    # points for bounds, and fixed semantic indices for facial features.
    main = pts[: min(len(pts), 468)]
    xs, ys, zs = main[:, 0], main[:, 1], main[:, 2]
    minx, maxx = float(xs.min()), float(xs.max())
    miny, maxy = float(ys.min()), float(ys.max())
    minz, maxz = float(zs.min()), float(zs.max())
    head_w = max(maxx - minx, 1e-6)
    head_h = max(maxy - miny, 1e-6)
    cx, cy = (minx + maxx) * 0.5, (miny + maxy) * 0.5

    head_ratio = head_w / head_h
    hw = clamp(0.645 * (head_ratio / 0.877), 0.56, 0.74)
    hh = clamp(0.755 + (0.877 - head_ratio) * 0.10, 0.68, 0.84)
    hd = clamp(0.50 + ((maxz - minz) / head_w - 0.12) * 0.55, 0.44, 0.60)

    l_eye = (p2(pts, 33) + p2(pts, 133)) * 0.5
    r_eye = (p2(pts, 263) + p2(pts, 362)) * 0.5
    eye_center = (l_eye + r_eye) * 0.5
    eye_sep = clamp((dist2(l_eye, r_eye) / head_w) * hw, 0.20, 0.33)
    eye_w = (dist2(p2(pts, 33), p2(pts, 133)) + dist2(p2(pts, 263), p2(pts, 362))) * 0.5
    eye_size = clamp((eye_w / head_w) * hw * 1.22, 0.10, 0.18)
    eye_y = clamp(((cy - float(eye_center[1])) / head_h) * hh * 1.22, 0.02, 0.20)

    left_ap = dist2(p2(pts, 159), p2(pts, 145)) / max(dist2(p2(pts, 33), p2(pts, 133)), 1e-6)
    right_ap = dist2(p2(pts, 386), p2(pts, 374)) / max(dist2(p2(pts, 263), p2(pts, 362)), 1e-6)
    eye_ap = (left_ap + right_ap) * 0.5
    blink = clamp((eye_ap - 0.045) / 0.18, 0.0, 1.0)

    nostril_span = dist2(p2(pts, 98), p2(pts, 327))
    nose_w = clamp((nostril_span / head_w) * hw * 0.92, 0.09, 0.17)
    nose_len = clamp((float(p2(pts, 1)[1] - p2(pts, 6)[1]) / head_h) * hh * 0.86, 0.10, 0.19)

    mouth_w = clamp((dist2(p2(pts, 61), p2(pts, 291)) / head_w) * hw, 0.16, 0.30)
    mouth_ctr = (p2(pts, 13) + p2(pts, 14)) * 0.5
    mouth_y = clamp(-((float(mouth_ctr[1]) - cy) / head_h) * hh * 1.24, -0.44, -0.20)
    mouth_gap = dist2(p2(pts, 13), p2(pts, 14)) / head_h
    neutral_mouth = calib.value("mouth_gap", 0.018)
    mouth_open = clamp((mouth_gap - neutral_mouth) * 14.0, 0.0, 1.0)

    corner_y = float((p2(pts, 61)[1] + p2(pts, 291)[1]) * 0.5)
    smile = clamp(((float(mouth_ctr[1]) - corner_y) / head_h) * 10.0, 0.0, 1.0)

    brow_pts = [70, 105, 107, 336, 334, 300]
    brow_y_img = float(np.mean([p2(pts, i)[1] for i in brow_pts if i < len(pts)]))
    eye_y_img = float(eye_center[1])
    brow_gap = (eye_y_img - brow_y_img) / head_h
    neutral_brow = calib.value("brow_gap", brow_gap)
    brow_raise = clamp((brow_gap - neutral_brow) * 10.0, 0.0, 1.0)
    brow_y = clamp(eye_y + brow_gap * hh * 0.95, 0.16, 0.34)

    yaw = clamp((float(p2(pts, 1)[0]) - float(eye_center[0])) / head_w * 1.20, -0.65, 0.65)
    pitch = clamp((cy - float(p2(pts, 1)[1])) / head_h * 0.20, -0.18, 0.18)

    metrics = {
        "mouth_gap": mouth_gap,
        "brow_gap": brow_gap,
        "eye_aperture": eye_ap,
    }
    params: dict[str, float | int | str] = {
        "type": "avatar",
        "name": name,
        "hw": hw,
        "hh": hh,
        "hd": hd,
        "eye_sep": eye_sep,
        "eye_y": eye_y,
        "eye_size": eye_size,
        "nose_len": nose_len,
        "nose_w": nose_w,
        "mouth_w": mouth_w,
        "mouth_y": mouth_y,
        "brow_y": brow_y,
        "blink": blink,
        "mouth_open": mouth_open,
        "smile": smile,
        "brow_raise": brow_raise,
        "lower_lid": clamp(0.36 - blink * 0.18, 0.12, 0.42),
        "yaw": yaw,
        "pitch": pitch,
    }
    return params, metrics


def extract_avatar_params_68(pts: np.ndarray, name: str, calib: NeutralCalibration) -> tuple[dict[str, float | int | str], dict[str, float]]:
    xs, ys = pts[:, 0], pts[:, 1]
    minx, maxx = float(xs.min()), float(xs.max())
    miny, maxy = float(ys.min()), float(ys.max())
    head_w = max(maxx - minx, 1e-6)
    head_h = max(maxy - miny, 1e-6)
    cx, cy = (minx + maxx) * 0.5, (miny + maxy) * 0.5

    head_ratio = head_w / head_h
    hw = clamp(0.645 * (head_ratio / 0.72), 0.56, 0.74)
    hh = clamp(0.755 + (0.72 - head_ratio) * 0.08, 0.68, 0.84)
    hd = 0.52

    l_eye = np.mean(pts[36:42, :2], axis=0)
    r_eye = np.mean(pts[42:48, :2], axis=0)
    eye_center = (l_eye + r_eye) * 0.5
    eye_sep = clamp((dist2(l_eye, r_eye) / head_w) * hw, 0.20, 0.33)
    eye_w = (dist2(pts[36], pts[39]) + dist2(pts[42], pts[45])) * 0.5
    eye_size = clamp((eye_w / head_w) * hw * 1.10, 0.10, 0.18)
    eye_y = clamp(((cy - float(eye_center[1])) / head_h) * hh * 1.16, 0.02, 0.20)

    left_ap = (dist2(pts[37], pts[41]) + dist2(pts[38], pts[40])) * 0.5 / max(dist2(pts[36], pts[39]), 1e-6)
    right_ap = (dist2(pts[43], pts[47]) + dist2(pts[44], pts[46])) * 0.5 / max(dist2(pts[42], pts[45]), 1e-6)
    eye_ap = (left_ap + right_ap) * 0.5
    blink = clamp((eye_ap - 0.045) / 0.18, 0.0, 1.0)

    nose_w = clamp((dist2(pts[31], pts[35]) / head_w) * hw * 0.92, 0.09, 0.17)
    nose_len = clamp((dist2(pts[27], pts[30]) / head_h) * hh * 0.90, 0.10, 0.19)

    mouth_w = clamp((dist2(pts[48], pts[54]) / head_w) * hw, 0.16, 0.30)
    mouth_ctr = (pts[62, :2] + pts[66, :2]) * 0.5
    mouth_y = clamp(-((float(mouth_ctr[1]) - cy) / head_h) * hh * 1.20, -0.44, -0.20)
    mouth_gap = dist2(pts[62], pts[66]) / head_h
    neutral_mouth = calib.value("mouth_gap", 0.014)
    mouth_open = clamp((mouth_gap - neutral_mouth) * 16.0, 0.0, 1.0)

    corner_y = float((pts[48, 1] + pts[54, 1]) * 0.5)
    smile = clamp(((float(mouth_ctr[1]) - corner_y) / head_h) * 9.0, 0.0, 1.0)

    brow_y_img = float(np.mean(pts[17:27, 1]))
    eye_y_img = float(eye_center[1])
    brow_gap = (eye_y_img - brow_y_img) / head_h
    neutral_brow = calib.value("brow_gap", brow_gap)
    brow_raise = clamp((brow_gap - neutral_brow) * 10.0, 0.0, 1.0)
    brow_y = clamp(eye_y + brow_gap * hh * 0.92, 0.16, 0.34)

    yaw = clamp((float(pts[30, 0]) - float(eye_center[0])) / head_w * 1.18, -0.65, 0.65)
    pitch = clamp((cy - float(pts[30, 1])) / head_h * 0.20, -0.18, 0.18)

    metrics = {
        "mouth_gap": mouth_gap,
        "brow_gap": brow_gap,
        "eye_aperture": eye_ap,
    }
    params: dict[str, float | int | str] = {
        "type": "avatar",
        "name": name,
        "hw": hw,
        "hh": hh,
        "hd": hd,
        "eye_sep": eye_sep,
        "eye_y": eye_y,
        "eye_size": eye_size,
        "nose_len": nose_len,
        "nose_w": nose_w,
        "mouth_w": mouth_w,
        "mouth_y": mouth_y,
        "brow_y": brow_y,
        "blink": blink,
        "mouth_open": mouth_open,
        "smile": smile,
        "brow_raise": brow_raise,
        "lower_lid": clamp(0.36 - blink * 0.18, 0.12, 0.42),
        "yaw": yaw,
        "pitch": pitch,
    }
    return params, metrics


def smooth_params(prev: dict[str, float | int | str] | None, cur: dict[str, float | int | str], alpha: float) -> dict[str, float | int | str]:
    if prev is None:
        return cur
    out = dict(cur)
    for key, value in cur.items():
        if isinstance(value, (int, float)) and isinstance(prev.get(key), (int, float)):
            out[key] = float(prev[key]) * (1.0 - alpha) + float(value) * alpha
    return out


def rounded_points(arr: np.ndarray) -> list[list[float]]:
    return np.round(arr.astype(np.float32), 6).tolist()


def draw_face_mesh(frame: np.ndarray, pts: np.ndarray, edges: Iterable[object]) -> None:
    h, w = frame.shape[:2]
    for edge in edges:
        a, b = edge_pair(edge)
        if a >= len(pts) or b >= len(pts):
            continue
        ax, ay = int(pts[a, 0] * w), int(pts[a, 1] * h)
        bx, by = int(pts[b, 0] * w), int(pts[b, 1] * h)
        cv2.line(frame, (ax, ay), (bx, by), (50, 190, 245), 1, cv2.LINE_AA)


def delaunay_triangles(pts: np.ndarray) -> list[tuple[int, int, int]]:
    if len(pts) < 3:
        return []
    scale = 1000.0
    coords = pts[:, :2] * scale
    subdiv = cv2.Subdiv2D((0, 0, int(scale), int(scale)))
    for p in coords:
        x = clamp(float(p[0]), 1.0, scale - 1.0)
        y = clamp(float(p[1]), 1.0, scale - 1.0)
        try:
            subdiv.insert((x, y))
        except cv2.error:
            pass

    out: set[tuple[int, int, int]] = set()
    for tri in subdiv.getTriangleList():
        verts = [(float(tri[0]), float(tri[1])), (float(tri[2]), float(tri[3])), (float(tri[4]), float(tri[5]))]
        idxs: list[int] = []
        for vx, vy in verts:
            d = np.square(coords[:, 0] - vx) + np.square(coords[:, 1] - vy)
            idx = int(np.argmin(d))
            if float(d[idx]) > 16.0:
                break
            idxs.append(idx)
        if len(idxs) == 3 and len(set(idxs)) == 3:
            out.add(tuple(sorted(idxs)))
    return sorted(out)


def draw_indexed_mesh(frame: np.ndarray, pts: np.ndarray, triangles: Iterable[tuple[int, int, int]]) -> None:
    h, w = frame.shape[:2]
    for a, b, c in triangles:
        if max(a, b, c) >= len(pts):
            continue
        pa = (int(pts[a, 0] * w), int(pts[a, 1] * h))
        pb = (int(pts[b, 0] * w), int(pts[b, 1] * h))
        pc = (int(pts[c, 0] * w), int(pts[c, 1] * h))
        cv2.line(frame, pa, pb, (50, 190, 245), 1, cv2.LINE_AA)
        cv2.line(frame, pb, pc, (50, 190, 245), 1, cv2.LINE_AA)
        cv2.line(frame, pc, pa, (50, 190, 245), 1, cv2.LINE_AA)
    for p in pts:
        cv2.circle(frame, (int(p[0] * w), int(p[1] * h)), 1, (255, 255, 255), -1, cv2.LINE_AA)


def draw_dense_points(frame: np.ndarray, pts: np.ndarray) -> None:
    if len(pts) == 0:
        return
    h, w = frame.shape[:2]
    xs = np.clip((pts[:, 0] * w).astype(np.int32), 0, w - 1)
    ys = np.clip((pts[:, 1] * h).astype(np.int32), 0, h - 1)
    frame[ys, xs] = (235, 195, 35)


def run_opencv(args: argparse.Namespace) -> int:
    if not args.lbf_model.exists():
        print(f"face_capture: missing OpenCV LBF model: {args.lbf_model}", file=sys.stderr)
        return 2

    face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
    if face_cascade.empty():
        print("face_capture: unable to load Haar face detector", file=sys.stderr)
        return 2

    facemark = cv2.face.createFacemarkLBF()
    facemark.loadModel(str(args.lbf_model))

    cap = cv2.VideoCapture(args.camera)
    if not cap.isOpened():
        print(f"face_capture: unable to open camera {args.camera}", file=sys.stderr)
        return 2
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    cap.set(cv2.CAP_PROP_FPS, args.fps)

    landmark_fp = None
    if args.landmark_file:
        args.landmark_file.parent.mkdir(parents=True, exist_ok=True)
        landmark_fp = args.landmark_file.open("a", encoding="utf-8")

    calib = NeutralCalibration(args.calibrate_frames)
    prev: dict[str, float | int | str] | None = None
    dense_dumped = 0
    frame_no = 0
    last_status = 0.0

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                print("face_capture: camera read failed", file=sys.stderr)
                break
            frame_no += 1
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            faces = face_cascade.detectMultiScale(gray, scaleFactor=1.12, minNeighbors=5, minSize=(90, 90))
            if len(faces) == 0:
                if args.preview:
                    cv2.putText(frame, "no face", (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 2)
                    cv2.imshow("dsco face capture", frame)
                    if cv2.waitKey(1) & 0xFF == ord("q"):
                        break
                continue

            faces = np.array(sorted(faces, key=lambda r: int(r[2]) * int(r[3]), reverse=True)[:1], dtype=np.int32)
            ok, landmarks = facemark.fit(gray, faces)
            if not ok or len(landmarks) == 0:
                continue

            raw_xy = np.asarray(landmarks[0], dtype=np.float32).reshape(-1, 2)
            h, w = frame.shape[:2]
            pts = np.column_stack((raw_xy[:, 0] / max(w, 1), raw_xy[:, 1] / max(h, 1), np.zeros(len(raw_xy), dtype=np.float32))).astype(np.float32)
            triangles = delaunay_triangles(pts)
            need_dense = args.preview or (
                landmark_fp and args.dump_every > 0 and frame_no % args.dump_every == 0 and dense_dumped < args.dump_max_frames
            )
            dense = dense_landmarks(pts, triangles, args.dense) if need_dense else None
            params, metrics = extract_avatar_params_68(pts, args.name, calib)
            calib.update(metrics)
            params = smooth_params(prev, params, clamp(args.smoothing, 0.0, 1.0))
            prev = params

            print(json.dumps(params, separators=(",", ":")), flush=True)

            if dense is not None and landmark_fp and args.dump_every > 0 and frame_no % args.dump_every == 0 and dense_dumped < args.dump_max_frames:
                rec = {
                    "backend": "opencv_lbf",
                    "frame": frame_no,
                    "time": time.time(),
                    "raw_count": int(len(pts)),
                    "dense_count": int(len(dense)),
                    "raw_landmarks": rounded_points(pts),
                    "dense_landmarks": rounded_points(dense),
                }
                landmark_fp.write(json.dumps(rec, separators=(",", ":")) + "\n")
                landmark_fp.flush()
                dense_dumped += 1

            now = time.time()
            if now - last_status > 1.0:
                print(
                    f"face_capture: backend=opencv_lbf raw={len(pts)} dense={args.dense} "
                    f"calib={min(calib.count, calib.frames)}/{calib.frames} "
                    f"blink={float(params['blink']):.2f} mouth={float(params['mouth_open']):.2f} "
                    f"dumped={dense_dumped}",
                    file=sys.stderr,
                )
                last_status = now

            if args.preview:
                if dense is not None:
                    draw_dense_points(frame, dense)
                draw_indexed_mesh(frame, pts, triangles)
                x, y, fw, fh = map(int, faces[0])
                cv2.rectangle(frame, (x, y), (x + fw, y + fh), (40, 220, 80), 1, cv2.LINE_AA)
                cv2.putText(
                    frame,
                    f"opencv_lbf raw {len(pts)} dense {args.dense} calib {min(calib.count, calib.frames)}/{calib.frames}",
                    (20, 32),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.75,
                    (255, 255, 255),
                    2,
                )
                cv2.imshow("dsco face capture", frame)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
    finally:
        if calib.count:
            args.calibration_file.write_text(json.dumps(calib.to_json(), indent=2) + "\n", encoding="utf-8")
        if landmark_fp:
            landmark_fp.close()
        cap.release()
        if args.preview:
            cv2.destroyAllWindows()

    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--camera", type=int, default=0)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--name", default="Arthur")
    ap.add_argument("--backend", choices=("opencv", "mediapipe"), default="opencv")
    ap.add_argument("--model", type=Path, default=Path("/tmp/dsco-face/face_landmarker.task"))
    ap.add_argument("--lbf-model", type=Path, default=Path("/tmp/dsco-face/lbfmodel.yaml"))
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--dense", type=int, default=10000)
    ap.add_argument("--landmark-file", type=Path)
    ap.add_argument("--dump-every", type=int, default=10)
    ap.add_argument("--dump-max-frames", type=int, default=300)
    ap.add_argument("--calibrate-frames", type=int, default=30)
    ap.add_argument("--calibration-file", type=Path, default=Path("/tmp/dsco_face_calibration.json"))
    ap.add_argument("--smoothing", type=float, default=0.35)
    args = ap.parse_args()

    if args.backend == "opencv":
        return run_opencv(args)

    if not args.model.exists():
        print(f"face_capture: missing MediaPipe model: {args.model}", file=sys.stderr)
        return 2

    tessellation = mp_vision.FaceLandmarksConnections.FACE_LANDMARKS_TESSELATION
    triangles = build_triangles(tessellation)
    cap = cv2.VideoCapture(args.camera)
    if not cap.isOpened():
        print(f"face_capture: unable to open camera {args.camera}", file=sys.stderr)
        return 2
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    cap.set(cv2.CAP_PROP_FPS, args.fps)

    landmark_fp = None
    if args.landmark_file:
        args.landmark_file.parent.mkdir(parents=True, exist_ok=True)
        landmark_fp = args.landmark_file.open("a", encoding="utf-8")

    calib = NeutralCalibration(args.calibrate_frames)
    prev: dict[str, float | int | str] | None = None
    dense_dumped = 0
    frame_no = 0
    last_status = 0.0

    options = mp_vision.FaceLandmarkerOptions(
        base_options=mp_tasks.BaseOptions(
            model_asset_path=str(args.model),
            delegate=mp_tasks.BaseOptions.Delegate.CPU,
        ),
        running_mode=mp_vision.RunningMode.VIDEO,
        num_faces=1,
        min_face_detection_confidence=0.55,
        min_face_presence_confidence=0.55,
        min_tracking_confidence=0.55,
        output_face_blendshapes=True,
        output_facial_transformation_matrixes=True,
    )

    with mp_vision.FaceLandmarker.create_from_options(options) as landmarker:
        try:
            while True:
                ok, frame = cap.read()
                if not ok:
                    print("face_capture: camera read failed", file=sys.stderr)
                    break
                frame_no += 1
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
                timestamp_ms = frame_no * max(1, int(1000 / max(args.fps, 1)))
                result = landmarker.detect_for_video(mp_image, timestamp_ms)
                if not result.face_landmarks:
                    if args.preview:
                        cv2.putText(frame, "no face", (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 2)
                        cv2.imshow("dsco face capture", frame)
                        if cv2.waitKey(1) & 0xFF == ord("q"):
                            break
                    continue

                face = result.face_landmarks[0]
                pts = np.array([[lm.x, lm.y, lm.z] for lm in face], dtype=np.float32)
                params, metrics = extract_avatar_params(pts, args.name, calib)
                calib.update(metrics)
                params = smooth_params(prev, params, clamp(args.smoothing, 0.0, 1.0))
                prev = params

                print(json.dumps(params, separators=(",", ":")), flush=True)

                if landmark_fp and args.dump_every > 0 and frame_no % args.dump_every == 0 and dense_dumped < args.dump_max_frames:
                    dense = dense_landmarks(pts, triangles, args.dense)
                    rec = {
                        "backend": "mediapipe_tasks",
                        "frame": frame_no,
                        "time": time.time(),
                        "raw_count": int(len(pts)),
                        "dense_count": int(len(dense)),
                        "raw_landmarks": rounded_points(pts),
                        "dense_landmarks": rounded_points(dense),
                    }
                    landmark_fp.write(json.dumps(rec, separators=(",", ":")) + "\n")
                    landmark_fp.flush()
                    dense_dumped += 1

                now = time.time()
                if now - last_status > 1.0:
                    print(
                        f"face_capture: raw={len(pts)} dense={args.dense} "
                        f"calib={min(calib.count, calib.frames)}/{calib.frames} "
                        f"blink={float(params['blink']):.2f} mouth={float(params['mouth_open']):.2f} "
                        f"dumped={dense_dumped}",
                        file=sys.stderr,
                    )
                    last_status = now

                if args.preview:
                    draw_face_mesh(frame, pts, tessellation)
                    cv2.putText(
                        frame,
                        f"raw {len(pts)} dense {args.dense} calib {min(calib.count, calib.frames)}/{calib.frames}",
                        (20, 32),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.75,
                        (255, 255, 255),
                        2,
                    )
                    cv2.imshow("dsco face capture", frame)
                    if cv2.waitKey(1) & 0xFF == ord("q"):
                        break
        finally:
            if calib.count:
                args.calibration_file.write_text(json.dumps(calib.to_json(), indent=2) + "\n", encoding="utf-8")
            if landmark_fp:
                landmark_fp.close()
            cap.release()
            if args.preview:
                cv2.destroyAllWindows()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
