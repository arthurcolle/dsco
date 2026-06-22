#!/usr/bin/env python3
"""
face_capture.py — MediaPipe FaceMesh → dsco avatar driver
=========================================================
Reads webcam, extracts 478 landmark mesh, maps geometry to
aface_t params, smooths with EMA, writes live JSON to stdout
(one line per frame) for --anim avatar to consume via stdin.

Usage:
  python3 face_capture.py | ./dsco --anim avatar '{"stdin_params":true,"width":120,"height":80,"aa":3,"fps":24}'

Or run standalone to preview the extracted params:
  python3 face_capture.py --preview
"""

import cv2, mediapipe as mp, json, sys, math, time, argparse, os, urllib.request
import numpy as np
from mediapipe.tasks.python import vision, BaseOptions

# ── MediaPipe Tasks setup ──────────────────────────────────────────────────
# This mediapipe build (0.10.x) ships only the Tasks API; the legacy
# mp.solutions.face_mesh module was removed. We use FaceLandmarker, which
# emits the same 478-point mesh, behind a small adapter so the geometry code
# below (extract/ear/lm) keeps using `landmarks.landmark[idx]`.
_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'face_landmarker.task')
_MODEL_URL  = ('https://storage.googleapis.com/mediapipe-models/face_landmarker/'
               'face_landmarker/float16/1/face_landmarker.task')

def _ensure_model():
    if not os.path.exists(_MODEL_PATH):
        sys.stderr.write("[face_capture] downloading face_landmarker.task ...\n")
        sys.stderr.flush()
        urllib.request.urlretrieve(_MODEL_URL, _MODEL_PATH)
    return _MODEL_PATH

class _MeshAdapter:
    """Wraps a Tasks landmark list so legacy code can use `.landmark[idx]`."""
    __slots__ = ('landmark',)
    def __init__(self, landmark_list):
        self.landmark = landmark_list

# FaceMesh landmark indices we care about
# See: https://github.com/google/mediapipe/blob/master/mediapipe/modules/face_geometry/data/canonical_face_model_uv_visualization.png
IDX = dict(
    # head bounding points
    top         = 10,
    bottom      = 152,
    left        = 234,
    right       = 454,
    # eyes
    l_eye_outer = 33,
    l_eye_inner = 133,
    l_eye_top   = 159,
    l_eye_bot   = 145,
    r_eye_outer = 362,
    r_eye_inner = 263,
    r_eye_top   = 386,
    r_eye_bot   = 374,
    # brows
    l_brow_peak = 105,
    r_brow_peak = 334,
    # nose
    nose_tip    = 1,
    nose_top    = 6,
    nose_l      = 129,
    nose_r      = 358,
    # mouth
    mouth_l     = 61,
    mouth_r     = 291,
    mouth_top   = 13,
    mouth_bot   = 14,
    # ears (approximate — outer cheek boundary)
    l_ear       = 234,
    r_ear       = 454,
    # chin
    chin        = 152,
)

def lm(landmarks, idx, w, h):
    """Return (x, y) in pixel coords for landmark index."""
    p = landmarks.landmark[idx]
    return np.array([p.x * w, p.y * h])

def dist(a, b):
    return float(np.linalg.norm(a - b))

def ema(prev, curr, alpha=0.15):
    """Exponential moving average — smooths jitter."""
    return {k: alpha * curr[k] + (1 - alpha) * prev.get(k, curr[k]) for k in curr}

# ── EAR (Eye Aspect Ratio) → blink ────────────────────────────────────────
def ear(landmarks, outer, inner, top, bot, w, h):
    A = dist(lm(landmarks, top, w, h), lm(landmarks, bot, w, h))
    B = dist(lm(landmarks, outer, w, h), lm(landmarks, inner, w, h))
    return A / (B + 1e-9)

def extract(landmarks, W, H):
    """Extract normalized face geometry from 478-point mesh."""
    def p(idx): return lm(landmarks, idx, W, H)

    # Head bounding box in pixels
    head_top    = p(IDX['top'])
    head_bot    = p(IDX['bottom'])
    head_left   = p(IDX['left'])
    head_right  = p(IDX['right'])

    head_h   = dist(head_top, head_bot)          # vertical
    head_w   = dist(head_left, head_right)        # horizontal
    head_ref = (head_h + head_w) / 2.0           # normalizer

    # Eye centers
    le_c = (p(IDX['l_eye_outer']) + p(IDX['l_eye_inner'])) * 0.5
    re_c = (p(IDX['r_eye_outer']) + p(IDX['r_eye_inner'])) * 0.5
    eye_center = (le_c + re_c) * 0.5

    # Eye separation (fraction of head width)
    eye_sep_px = dist(le_c, re_c)
    eye_sep = 0.10 + (eye_sep_px / head_w) * 0.25   # map [0,1] → [0.10,0.35]

    # Eye vertical position relative to head center
    head_center_y = (head_top[1] + head_bot[1]) * 0.5
    eye_y_px = eye_center[1] - head_center_y         # negative = above center
    eye_y = -(eye_y_px / head_h) * 0.6 + 0.10       # avatar: positive=up

    # Eye size
    l_ear_val = ear(landmarks, IDX['l_eye_outer'], IDX['l_eye_inner'],
                    IDX['l_eye_top'], IDX['l_eye_bot'], W, H)
    r_ear_val = ear(landmarks, IDX['r_eye_outer'], IDX['r_eye_inner'],
                    IDX['r_eye_top'], IDX['r_eye_bot'], W, H)
    avg_ear = (l_ear_val + r_ear_val) * 0.5
    # EAR ~0.25-0.35 = open, ~0.15 = squinting, <0.12 = closed
    blink = max(0.0, min(1.0, (avg_ear - 0.12) / 0.20))
    l_eye_w_px = dist(p(IDX['l_eye_outer']), p(IDX['l_eye_inner']))
    eye_size = 0.08 + (l_eye_w_px / head_w) * 0.12

    # Nose
    nose_tip_px  = p(IDX['nose_tip'])
    nose_top_px  = p(IDX['nose_top'])
    nose_l_px    = p(IDX['nose_l'])
    nose_r_px    = p(IDX['nose_r'])
    nose_len_px  = dist(nose_tip_px, nose_top_px)
    nose_w_px    = dist(nose_l_px, nose_r_px)
    nose_len = 0.10 + (nose_len_px / head_h) * 0.15
    nose_w   = 0.07 + (nose_w_px / head_w) * 0.10

    # Mouth
    mouth_l_px = p(IDX['mouth_l'])
    mouth_r_px = p(IDX['mouth_r'])
    mouth_w_px = dist(mouth_l_px, mouth_r_px)
    mouth_c    = (mouth_l_px + mouth_r_px) * 0.5
    mouth_y_px = mouth_c[1] - head_center_y
    mouth_w   = 0.15 + (mouth_w_px / head_w) * 0.15
    mouth_y   = -(mouth_y_px / head_h) * 0.6 - 0.05

    # Brows
    l_brow = p(IDX['l_brow_peak'])
    r_brow = p(IDX['r_brow_peak'])
    brow_c_y = (l_brow[1] + r_brow[1]) * 0.5
    brow_y_px = brow_c_y - head_center_y
    brow_y = -(brow_y_px / head_h) * 0.6 + 0.05
    # clamp brow above eyes
    brow_y = max(eye_y + 0.10, brow_y)

    # Head proportions → hw, hh, hd
    hw = 0.60 + (head_w / (head_w + head_h)) * 0.20   # width radius [0.60..0.80]
    hh = 0.70 + (head_h / (head_w + head_h)) * 0.20   # height radius
    hd = 0.72  # depth — not measurable from 2D front view, keep default

    # Ears (approx from face width)
    ear_size = 0.13 + (head_w / max(W, H)) * 0.12

    # Yaw / pitch from nose deviation from face center
    face_cx = (head_left[0] + head_right[0]) * 0.5
    face_cy = (head_top[1] + head_bot[1]) * 0.5
    nose_x   = nose_tip_px[0]
    nose_y   = nose_tip_px[1]
    yaw   = ((nose_x - face_cx) / (head_w * 0.5)) * 0.6
    pitch = -((nose_y - face_cy) / (head_h * 0.5)) * 0.4

    return dict(
        hw=round(hw, 4), hh=round(hh, 4), hd=round(hd, 4),
        eye_sep=round(eye_sep, 4), eye_y=round(eye_y, 4), eye_size=round(eye_size, 4),
        nose_len=round(nose_len, 4), nose_w=round(nose_w, 4),
        mouth_w=round(mouth_w, 4), mouth_y=round(mouth_y, 4),
        brow_y=round(brow_y, 4), ear_size=round(ear_size, 4),
        blink=round(blink, 4),
        yaw=round(yaw, 4), pitch=round(pitch, 4),
    )

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--preview', action='store_true', help='Show OpenCV window with landmarks')
    ap.add_argument('--cam', type=int, default=0, help='Camera index')
    ap.add_argument('--alpha', type=float, default=0.18, help='EMA smoothing (0.05=smooth, 0.5=responsive)')
    ap.add_argument('--name', type=str, default='Arthur', help='Avatar name (sets palette)')
    ap.add_argument('--skin', type=int, default=1)
    ap.add_argument('--iris', type=int, default=0)
    ap.add_argument('--hair', type=int, default=1)
    ap.add_argument('--haircol', type=int, default=1)
    args = ap.parse_args()

    cap = cv2.VideoCapture(args.cam)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

    smoothed = {}
    # Baseline: one calm frame to set initial geometry
    baseline = None
    baseline_frames = 0

    options = vision.FaceLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=_ensure_model()),
        running_mode=vision.RunningMode.VIDEO,
        num_faces=1,
        min_face_detection_confidence=0.5,
        min_tracking_confidence=0.5,
    )

    with vision.FaceLandmarker.create_from_options(options) as face_mesh:

        frame_idx = 0
        while True:
            ok, frame = cap.read()
            if not ok:
                break

            H, W = frame.shape[:2]
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
            ts_ms = int(frame_idx * 1000 / 30)
            frame_idx += 1
            results = face_mesh.detect_for_video(mp_image, ts_ms)

            if results.face_landmarks:
                lms = _MeshAdapter(results.face_landmarks[0])
                params = extract(lms, W, H)

                # Calibrate baseline from first 30 frames (neutral face)
                if baseline_frames < 30:
                    if baseline is None:
                        baseline = params.copy()
                    else:
                        baseline = ema(baseline, params, 0.1)
                    baseline_frames += 1
                    if baseline_frames == 30:
                        smoothed = baseline.copy()
                        sys.stderr.write("[face_capture] baseline calibrated\n")
                        sys.stderr.flush()

                # Apply EMA smoothing
                if smoothed:
                    smoothed = ema(smoothed, params, args.alpha)
                else:
                    smoothed = params.copy()

                # Build full JSON for dsco --anim avatar
                out = dict(smoothed)
                out['name']    = args.name
                out['skin']    = args.skin
                out['iris']    = args.iris
                out['hair']    = args.hair
                out['haircol'] = args.haircol

                if args.preview:
                    # Draw mesh overlay — scatter all 478 landmarks
                    for p in lms.landmark:
                        cv2.circle(frame, (int(p.x * W), int(p.y * H)), 1, (0, 255, 80), -1)
                    # HUD
                    cv2.putText(frame, f"blink:{out['blink']:.2f}  yaw:{out['yaw']:.2f}  pitch:{out['pitch']:.2f}",
                                (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 80), 1)
                    cv2.putText(frame, f"eye_sep:{out['eye_sep']:.3f}  nose_w:{out['nose_w']:.3f}  mouth_w:{out['mouth_w']:.3f}",
                                (10, 55), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 80), 1)
                    if baseline_frames < 30:
                        cv2.putText(frame, f"CALIBRATING {baseline_frames}/30 — hold neutral face",
                                    (10, 90), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 200, 255), 2)
                    cv2.imshow('face_capture — dsco avatar driver', frame)
                    if cv2.waitKey(1) & 0xFF == ord('q'):
                        break

                # Emit JSON line for dsco to consume
                print(json.dumps(out), flush=True)

            else:
                if args.preview:
                    cv2.putText(frame, "no face detected", (10, 30),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 255), 2)
                    cv2.imshow('face_capture — dsco avatar driver', frame)
                    if cv2.waitKey(1) & 0xFF == ord('q'):
                        break

    cap.release()
    if args.preview:
        cv2.destroyAllWindows()

if __name__ == '__main__':
    main()
