#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "${HOME}/.local/bin" "${HOME}/lais/bin" "${HOME}/lais/configs" "${HOME}/lais/data" "${HOME}/lais/chats" "${HOME}/lais/models"
BIN_SRC="${DIR}/build/lais"
[ -f "${BIN_SRC}" ] || BIN_SRC="${DIR}/lais"
if [ -f "${BIN_SRC}" ]; then
    if [ ! "${BIN_SRC}" -ef "${HOME}/.local/bin/lais" ]; then
        cp -f "${BIN_SRC}" "${HOME}/.local/bin/lais"
    fi
    if [ ! "${BIN_SRC}" -ef "${HOME}/lais/bin/lais" ]; then
        cp -f "${BIN_SRC}" "${HOME}/lais/bin/lais"
    fi
    chmod 0755 "${HOME}/.local/bin/lais" "${HOME}/lais/bin/lais"
    if command -v sudo >/dev/null 2>&1; then
        sudo cp -f "${BIN_SRC}" /usr/local/bin/lais 2>/dev/null && sudo chmod 0755 /usr/local/bin/lais 2>/dev/null || true
    fi
fi
for cfg in settings.json sources.json models.json en-us.lang ko-kr.lang; do
    if [ -f "${DIR}/configs/${cfg}" ] && [ ! -f "${HOME}/lais/configs/${cfg}" ]; then
        cp -f "${DIR}/configs/${cfg}" "${HOME}/lais/configs/${cfg}"
    fi
done
if [ -f "${DIR}/data/user.json" ] && [ ! -f "${HOME}/lais/data/user.json" ]; then
    cp -f "${DIR}/data/user.json" "${HOME}/lais/data/user.json"
    chmod 0600 "${HOME}/lais/data/user.json" 2>/dev/null || true
fi
if [ -d "${DIR}/bin" ] && [ ! "${DIR}/bin" -ef "${HOME}/lais/bin" ]; then
    cp -P -f "${DIR}/bin/"* "${HOME}/lais/bin/" 2>/dev/null || true
    chmod 0755 "${HOME}/lais/bin/"* 2>/dev/null || true
fi
echo "[OK] Restored LAIS binary, configs, PAKE user store, and pre-built engines to ~/lais!"
