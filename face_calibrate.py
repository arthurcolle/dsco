#!/usr/bin/env python3
"""
face_calibrate.py — measure your face once, print a stable JSON profile.
Run once, paste the output into --anim avatar '...' as fixed geometry.
"""
import cv2, mediapipe as mp, json, sys
import numpy as np
sys.path.insert(0, '/Users/arthurcolle/dsco-cli')
from face_capture import extract, ema, IDX, _MeshAdapter, _ensure_model
from mediapipe.tasks.python import vision, BaseOptions

N_FRAMES = 90

cap = cv2.VideoCapture(0)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

print("Hold a NEUTRAL expression — calibrating for 3 seconds...", file=sys.stderr)
accum = None
n = 0

_opts = vision.FaceLandmarkerOptions(
    base_options=BaseOptions(model_asset_path=_ensure_model()),
    running_mode=vision.RunningMode.VIDEO, num_faces=1,
    min_face_detection_confidence=0.6, min_tracking_confidence=0.6)

with vision.FaceLandmarker.create_from_options(_opts) as fm:
    frame_idx = 0
    while n < N_FRAMES:
        ok, frame = cap.read()
        if not ok: break
        H, W = frame.shape[:2]
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        res = fm.detect_for_video(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb),
                                  int(frame_idx * 1000 / 30))
        frame_idx += 1
        if res.face_landmarks:
            p = extract(_MeshAdapter(res.face_landmarks[0]), W, H)
            accum = ema(accum, p, 0.15) if accum else p
            n += 1
            print(f"\r  captured {n}/{N_FRAMES}", end='', flush=True, file=sys.stderr)

cap.release()
print(file=sys.stderr)

if accum:
    # Remove pose (let anim drive it)
    accum.pop('yaw', None)
    accum.pop('pitch', None)
    accum.pop('blink', None)
    # Add identity params
    accum['name'] = 'Arthur'
    accum['skin'] = 1
    accum['iris'] = 0
    accum['hair'] = 1
    accum['haircol'] = 1
    accum['view'] = 'face'
    accum['width'] = 120
    accum['height'] = 80
    accum['aa'] = 4
    accum['zoom'] = 1.3
    accum['fps'] = 24
    print("\n── Calibrated profile ──", file=sys.stderr)
    print(json.dumps(accum, indent=2), file=sys.stderr)
    print("\n── Run this: ──", file=sys.stderr)
    j = json.dumps(accum)
    print(f"./dsco --anim avatar '{j}'", file=sys.stderr)
    # Also write to file
    with open('face_profile.json', 'w') as f:
        json.dump(accum, f, indent=2)
    print("\nSaved to face_profile.json", file=sys.stderr)
else:
    print("ERROR: no face detected", file=sys.stderr)
    sys.exit(1)
