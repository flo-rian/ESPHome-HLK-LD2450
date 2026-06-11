#!/usr/bin/env bash
# setup-build-env.sh
# -----------------------------------------------------------------------------
# Sets up an isolated Python environment for compiling the
# ESPHome-HLK-LD2450 component. Mirrors .github/workflows/ci.yaml:
#   * Python 3.11 in a local virtualenv at .venv/
#   * esphome from PyPI (latest by default, or pinned via --esphome-version)
#   * optional lint/format tools (black, isort, yamllint, pre-commit)
#   * optional clang-format 13 via apt-get (Linux only; matches CI)
#
# Re-running the script reuses the existing venv and upgrades packages in
# place. ESPHome pulls PlatformIO + the ESP32 toolchain on first compile
# (cached at ~/.platformio).
# -----------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VENV_DIR="${VENV_DIR:-.venv}"
ESPHOME_VERSION="${ESPHOME_VERSION:-}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
WITH_LINT=0
WITH_CLANG_FORMAT=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --esphome-version VER   Pin a specific ESPHome version (default: latest)
  --python BIN            Python interpreter to use (default: python3)
  --venv DIR              Virtualenv directory (default: .venv)
  --with-lint             Install Python lint/format tools
                          (black, isort, yamllint, pre-commit)
  --with-clang-format     Install clang-format 13 via apt-get (Linux only)
  -h, --help              Show this help

Examples:
  $(basename "$0")
  $(basename "$0") --esphome-version 2024.12.0
  $(basename "$0") --with-lint --with-clang-format
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --esphome-version)   ESPHOME_VERSION="$2"; shift 2 ;;
    --python)            PYTHON_BIN="$2"; shift 2 ;;
    --venv)              VENV_DIR="$2"; shift 2 ;;
    --with-lint)         WITH_LINT=1; shift ;;
    --with-clang-format) WITH_CLANG_FORMAT=1; shift ;;
    -h|--help)           usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

log()  { printf '\033[1;34m[setup]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*" >&2; }

# ---- Environment hints ------------------------------------------------------
if [[ -n "${SUPERVISOR_TOKEN:-}" ]] || [[ -f /etc/hassio.json ]]; then
  warn "Detected a Home Assistant Supervisor container."
  warn "  - This script creates an isolated venv so the build does not depend"
  warn "    on HA's bundled Python or the ESPHome add-on's pinned packages."
  warn "  - To reproduce an error from inside the HA add-on, you may need to"
  warn "    pass --esphome-version <ver> matching the add-on's bundled ESPHome."
fi

# ---- Python -----------------------------------------------------------------
if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
  echo "error: $PYTHON_BIN not found. Install Python 3.11+ or pass --python." >&2
  exit 1
fi
PY_VERSION="$($PYTHON_BIN -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])')"
log "Python: $PY_VERSION  ($($PYTHON_BIN -c 'import sys; print(sys.executable)'))"

if ! $PYTHON_BIN -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'; then
  warn "Python 3.10+ is required. Found $PY_VERSION. Re-run with --python."
  exit 1
fi

# ---- Virtualenv -------------------------------------------------------------
if [[ ! -d "$VENV_DIR" ]]; then
  log "Creating virtualenv at $VENV_DIR"
  $PYTHON_BIN -m venv "$VENV_DIR"
else
  log "Reusing existing virtualenv at $VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"

log "Upgrading pip"
python -m pip install --upgrade pip

# ---- ESPHome (mirrors CI: requirements.txt then explicit install) ----------
log "Installing from requirements.txt"
python -m pip install -r requirements.txt

if [[ -n "$ESPHOME_VERSION" ]]; then
  log "Pinning ESPHome to $ESPHOME_VERSION"
  python -m pip install "esphome==$ESPHOME_VERSION"
else
  log "Upgrading ESPHome to latest (matches CI default)"
  python -m pip install -U esphome
fi

log "esphome $(esphome version)"

# ---- Optional tools ---------------------------------------------------------
if [[ $WITH_LINT -eq 1 ]]; then
  log "Installing lint/format tools (black, isort, yamllint, pre-commit)"
  python -m pip install -U 'black~=24.1' isort yamllint pre-commit
fi

if [[ $WITH_CLANG_FORMAT -eq 1 ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    log "Installing clang-format 13 via apt-get"
    if command -v sudo >/dev/null 2>&1 && [[ $EUID -ne 0 ]]; then
      sudo apt-get update
      sudo apt-get install -y clang-format-13
    else
      apt-get update
      apt-get install -y clang-format-13
    fi
  else
    warn "clang-format install via apt-get is Linux only; skipping"
  fi
fi

# ---- Summary ----------------------------------------------------------------
cat <<EOF

Setup complete.

To compile:
  source $VENV_DIR/bin/activate
  esphome compile tests/base.yaml     # minimal sanity build
  esphome compile tests/full.yaml     # full configuration

ESPHome will fetch PlatformIO and the ESP32 toolchain on first compile
(cached at ~/.platformio).
EOF
