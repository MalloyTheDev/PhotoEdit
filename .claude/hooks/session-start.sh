#!/bin/bash
#
# PhotoEdit — SessionStart hook for Claude Code on the web.
#
# Ensures the build toolchain is present and pre-builds the headless engine core
# (pe_core) so that unit tests and the formatter work immediately in the session.
# The Qt6 app shell needs Qt and is built in CI on Windows; it is intentionally
# NOT built here (the engine core is the part that is testable headless on Linux).
#
# Synchronous (no async block): guarantees the build is ready before the session
# starts, at the cost of a few seconds of startup. Idempotent and non-interactive.
set -euo pipefail

# Web sessions only; do nothing on a local machine.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel)}"

log() { echo "[photoedit setup] $*"; }

# 1. Ensure the build toolchain. These are usually already in the base image, so
#    only touch apt when something is actually missing (fast common case).
missing=0
for t in cmake ninja g++ clang-format; do
  command -v "$t" >/dev/null 2>&1 || missing=1
done
if [ "$missing" -eq 1 ]; then
  log "Installing build tools via apt..."
  sudo apt-get update -y >/tmp/photoedit-apt.log 2>&1
  sudo apt-get install -y --no-install-recommends \
    cmake ninja-build g++ clang-format >>/tmp/photoedit-apt.log 2>&1
fi
log "$(cmake --version | head -1); ninja $(ninja --version); $(g++ --version | head -1)"

# 2. Configure + build the headless engine core so `ctest` is ready to run.
log "Configuring + building engine core (pe_core)..."
if ! cmake --preset linux-core-dev >/tmp/photoedit-configure.log 2>&1; then
  log "Configure FAILED — last lines:"; tail -n 20 /tmp/photoedit-configure.log
  exit 1
fi
if ! cmake --build build/linux-core-dev >/tmp/photoedit-build.log 2>&1; then
  log "Build FAILED — last lines:"; tail -n 30 /tmp/photoedit-build.log
  exit 1
fi

log "Engine core built. Useful commands:"
log "  Run tests:     ctest --preset linux-core-dev"
log "  Rebuild:       cmake --build build/linux-core-dev"
log "  Format a file: clang-format -i <path>"
