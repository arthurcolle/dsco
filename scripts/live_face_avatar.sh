#!/usr/bin/env zsh
set -o pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
SCRIPT_PARENT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${DSCO_BIN:-}" ]]; then
    if [[ -x "$SCRIPT_PARENT/dsco" && -f "$SCRIPT_PARENT/face_capture.py" ]]; then
        DSCO_BIN="$SCRIPT_PARENT/dsco"
    elif [[ -x "$SCRIPT_DIR/dsco" ]]; then
        DSCO_BIN="$SCRIPT_DIR/dsco"
    else
        DSCO_BIN="$(command -v dsco 2>/dev/null || true)"
    fi
fi

if [[ -z "${DSCO_FACE_CAPTURE_SCRIPT:-}" ]]; then
    if [[ -f "$SCRIPT_PARENT/face_capture.py" ]]; then
        DSCO_FACE_CAPTURE_SCRIPT="$SCRIPT_PARENT/face_capture.py"
    else
        DSCO_FACE_CAPTURE_SCRIPT="$SCRIPT_PARENT/share/dsco/face_capture.py"
    fi
fi

if [[ -z "$DSCO_BIN" || ! -x "$DSCO_BIN" ]]; then
    echo "Missing dsco binary. Set DSCO_BIN or install dsco first." >&2
    exit 2
fi
if [[ ! -f "$DSCO_FACE_CAPTURE_SCRIPT" ]]; then
    echo "Missing face capture script: $DSCO_FACE_CAPTURE_SCRIPT" >&2
    echo "Set DSCO_FACE_CAPTURE_SCRIPT or run make install from the dsco checkout." >&2
    exit 2
fi

if [[ -z "${DSCO_FACE_PYTHON:-}" ]]; then
    for candidate in \
        "$HOME/.dsco/face-venv/bin/python" \
        "$HOME/.local/share/mise/installs/python/3.12.12/bin/python3.12" \
        "$(command -v python3.12 2>/dev/null || true)" \
        "$(command -v python3 2>/dev/null || true)"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            DSCO_FACE_PYTHON="$candidate"
            break
        fi
    done
fi
if [[ -z "$DSCO_FACE_PYTHON" || ! -x "$DSCO_FACE_PYTHON" ]]; then
    echo "Missing Python interpreter for face capture. Set DSCO_FACE_PYTHON." >&2
    exit 2
fi

mkdir -p /tmp/dsco-face/mpl
export MPLCONFIGDIR=/tmp/dsco-face/mpl

: "${DSCO_FACE_CAMERA:=0}"
: "${DSCO_FACE_BACKEND:=opencv}"
: "${DSCO_FACE_DENSE:=10000}"
: "${DSCO_FACE_FPS:=14}"
: "${DSCO_FACE_WIDTH:=1280}"
: "${DSCO_FACE_HEIGHT:=720}"
: "${DSCO_FACE_DUMP_EVERY:=4}"
: "${DSCO_FACE_CALIBRATE_FRAMES:=8}"
: "${DSCO_FACE_SMOOTHING:=0.62}"
: "${DSCO_AVATAR_WIDTH:=96}"
: "${DSCO_AVATAR_HEIGHT:=64}"
: "${DSCO_AVATAR_ZOOM:=1.08}"
: "${DSCO_AVATAR_NAME:=Arthur}"

LANDMARK_FILE="${DSCO_FACE_LANDMARK_FILE:-/tmp/dsco_face_landmarks.jsonl}"
CAPTURE_LOG="${DSCO_FACE_CAPTURE_LOG:-/tmp/dsco_face_capture.log}"
MODEL_FILE="${DSCO_FACE_MODEL:-/tmp/dsco-face/face_landmarker.task}"
LBF_MODEL_FILE="${DSCO_FACE_LBF_MODEL:-/tmp/dsco-face/lbfmodel.yaml}"

if [[ "$DSCO_FACE_BACKEND" == "mediapipe" && ! -s "$MODEL_FILE" ]]; then
    echo "Missing MediaPipe Face Landmarker model: $MODEL_FILE" >&2
    echo "Download the official model, or set DSCO_FACE_MODEL to another .task path." >&2
    exit 2
fi
if [[ "$DSCO_FACE_BACKEND" == "opencv" && ! -s "$LBF_MODEL_FILE" ]]; then
    echo "Missing OpenCV FacemarkLBF model: $LBF_MODEL_FILE" >&2
    echo "Download lbfmodel.yaml, or set DSCO_FACE_LBF_MODEL to another path." >&2
    exit 2
fi
AVATAR_SPEC=$(printf '{"kind":"avatar","stdin_params":true,"view":"face","width":%s,"height":%s,"aa":1,"zoom":%s,"skin":1,"hair":0,"color":true,"fps":%s}' \
    "$DSCO_AVATAR_WIDTH" "$DSCO_AVATAR_HEIGHT" "$DSCO_AVATAR_ZOOM" "$DSCO_FACE_FPS")

"$DSCO_FACE_PYTHON" "$DSCO_FACE_CAPTURE_SCRIPT" \
    --camera "$DSCO_FACE_CAMERA" \
    --backend "$DSCO_FACE_BACKEND" \
    --model "$MODEL_FILE" \
    --lbf-model "$LBF_MODEL_FILE" \
    --preview \
    --dense "$DSCO_FACE_DENSE" \
    --landmark-file "$LANDMARK_FILE" \
    --dump-every "$DSCO_FACE_DUMP_EVERY" \
    --calibrate-frames "$DSCO_FACE_CALIBRATE_FRAMES" \
    --smoothing "$DSCO_FACE_SMOOTHING" \
    --width "$DSCO_FACE_WIDTH" \
    --height "$DSCO_FACE_HEIGHT" \
    --fps "$DSCO_FACE_FPS" \
    --name "$DSCO_AVATAR_NAME" \
    2>"$CAPTURE_LOG" \
    | "$DSCO_BIN" --anim "$AVATAR_SPEC"
