#!/usr/bin/env bash
# ==============================================================================
# LAIS (Local AI Studio) v3.7.3 - Parallel Modular C++20 Builder & Packager
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

LAIS_DIR="${LAIS_DIR:-${HOME}/lais}"
CONFIGS_DIR="${LAIS_DIR}/configs"
DATA_DIR="${LAIS_DIR}/data"
CHATS_DIR="${LAIS_DIR}/chats"
MODELS_DIR="${LAIS_DIR}/models"
OUTPUTS_DIR="${LAIS_DIR}/outputs"
LOGS_DIR="${LAIS_DIR}/logs"
ENGINES_DIR="${LAIS_DIR}/engines"
BIN_DIR="${LAIS_DIR}/bin"
CACHE_DIR="${LAIS_DIR}/cache"
COMPAT_DIR="${CACHE_DIR}/cuda_compat"
BUILD_OUT_DIR="${SCRIPT_DIR}/build"
OBJ_DIR="${BUILD_OUT_DIR}/obj"

FORCE_REBUILD=0
LAIS_ONLY=0

for arg in "$@"; do
    case "${arg}" in
        --rebuild|--force)
            FORCE_REBUILD=1
            ;;
        --lais-only)
            LAIS_ONLY=1
            ;;
        -h|--help)
            echo "Usage: bash build.sh [--rebuild] [--lais-only]"
            exit 0
            ;;
    esac
done

mkdir -p "${LAIS_DIR}" "${CONFIGS_DIR}" "${DATA_DIR}" "${CHATS_DIR}" "${MODELS_DIR}" \
         "${OUTPUTS_DIR}" "${LOGS_DIR}" "${ENGINES_DIR}" \
         "${BIN_DIR}/configs" "${BIN_DIR}/data" "${CACHE_DIR}/tmp" "${COMPAT_DIR}/crt" \
         "${BUILD_OUT_DIR}" "${OBJ_DIR}"

safe_copy_file() {
    local src="$1"
    local dst="$2"
    local mode="${3:-if_missing}"
    if [ ! -f "${src}" ]; then
        return 0
    fi
    mkdir -p "$(dirname "${dst}")"
    if [ "${mode}" = "if_missing" ] && [ -f "${dst}" ] && [ -s "${dst}" ]; then
        return 0
    fi
    local src_real dst_real
    src_real="$(realpath -m "${src}" 2>/dev/null || echo "${src}")"
    dst_real="$(realpath -m "${dst}" 2>/dev/null || echo "${dst}")"
    if [ "${src_real}" = "${dst_real}" ] || [ "${src}" -ef "${dst}" ]; then
        return 0
    fi
    cp -f "${src}" "${dst}"
}

for cfg_name in settings.json sources.json models.json en-us.lang ko-kr.lang; do
    safe_copy_file "${SCRIPT_DIR}/configs/${cfg_name}" "${CONFIGS_DIR}/${cfg_name}" "if_missing"
done

# Upgrade only older catalogs/engine sources; retain a backup and same-version edits.
for cfg_name in models.json sources.json; do
    version_key=catalog_version
    [ "${cfg_name}" != sources.json ] || version_key=engine_config_version
    seed_cfg="${SCRIPT_DIR}/configs/${cfg_name}"
    installed_cfg="${CONFIGS_DIR}/${cfg_name}"
    if [ -f "${seed_cfg}" ] && [ -f "${installed_cfg}" ] && [ ! "${seed_cfg}" -ef "${installed_cfg}" ]; then
        seed_version="$(sed -nE "s/.*\"${version_key}\"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p" "${seed_cfg}" | head -n 1)"
        installed_version="$(sed -nE "s/.*\"${version_key}\"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p" "${installed_cfg}" | head -n 1)"
        needs_repair=0
        if [ "${cfg_name}" = "models.json" ] && [ "${seed_version:-0}" -eq "${installed_version:-0}" ]; then
            if grep -q '"llm_std_high"' "${seed_cfg}" && ! grep -q '"llm_std_high"' "${installed_cfg}"; then
                needs_repair=1
            elif ! grep -q '587cfbf3dc3eb82a594366b0fc9a7d6d4ec1c5b3' "${seed_cfg}" && grep -qE '587cfbf3dc3eb82a594366b0fc9a7d6d4ec1c5b3|LeeAeron/' "${installed_cfg}"; then
                needs_repair=1
            fi
        fi
        if [ "${seed_version:-0}" -gt "${installed_version:-0}" ] || [ "${needs_repair}" -eq 1 ]; then
            safe_copy_file "${installed_cfg}" "${installed_cfg}.pre-${seed_version}.bak" "if_missing"
            safe_copy_file "${seed_cfg}" "${installed_cfg}" "overwrite"
        fi
    fi
done

for cfg_name in settings.json sources.json models.json en-us.lang ko-kr.lang; do
    if [ -f "${CONFIGS_DIR}/${cfg_name}" ]; then
        safe_copy_file "${CONFIGS_DIR}/${cfg_name}" "${BIN_DIR}/configs/${cfg_name}" "overwrite"
    else
        safe_copy_file "${SCRIPT_DIR}/configs/${cfg_name}" "${BIN_DIR}/configs/${cfg_name}" "overwrite"
    fi
done

safe_copy_file "${SCRIPT_DIR}/data/user.json" "${DATA_DIR}/user.json" "if_missing"
if [ -f "${DATA_DIR}/user.json" ]; then
    chmod 0600 "${DATA_DIR}/user.json" 2>/dev/null || true
    safe_copy_file "${DATA_DIR}/user.json" "${BIN_DIR}/data/user.json" "overwrite"
elif [ -f "${SCRIPT_DIR}/data/user.json" ]; then
    safe_copy_file "${SCRIPT_DIR}/data/user.json" "${BIN_DIR}/data/user.json" "overwrite"
fi

SOURCES_JSON="${CONFIGS_DIR}/sources.json"
[ -f "${SOURCES_JSON}" ] || SOURCES_JSON="${SCRIPT_DIR}/configs/sources.json"

read_source_key() {
    python3 - "${SOURCES_JSON}" "$1" << 'PYEOF'
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
k = sys.argv[2]
if p.exists():
    data = json.loads(p.read_text(encoding="utf-8"))
    print(data.get(k, ""))
PYEOF
}

LLAMA_REPO="$(read_source_key llama_cpp_repo)"
LLAMA_MIN_BUILD="$(read_source_key llama_cpp_min_build)"
[ -n "${LLAMA_MIN_BUILD}" ] || LLAMA_MIN_BUILD=10261
SD_REPO="$(read_source_key stable_diffusion_cpp_repo)"
SD_VERIFIED_TAG="$(read_source_key stable_diffusion_cpp_tag)"
ACE_REPO="$(read_source_key acestep_cpp_repo)"
CUDA_KEYRING_URL="$(read_source_key cuda_keyring_deb_url)"
CUDA_APT_PKG="$(read_source_key cuda_apt_package)"

echo -e "\033[1;36m==============================================================================\033[0m"
echo -e "\033[1;36m LAIS (Local AI Studio) v3.7.3 - Modular C++20 Builder & Backup Packager\033[0m"
echo -e "\033[1;36m==============================================================================\033[0m"

if [ "${LAIS_ONLY}" -eq 0 ] || ! command -v g++ >/dev/null 2>&1; then
    if command -v sudo >/dev/null 2>&1; then
        echo -e "\033[1;34m[1/5]\033[0m Checking system build, CUDA host compiler (gcc-14/g++-14), and zip packages..."
        sudo DEBIAN_FRONTEND=noninteractive apt-get update
        sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            ca-certificates gnupg curl wget git build-essential cmake ninja-build pkg-config ccache \
            zstd zip unzip ffmpeg libgomp1 libgl1 libglib2.0-0t64 libsndfile1
        sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-14 g++-14 2>/dev/null || \
        sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-13 g++-13 2>/dev/null || true

        if [ -d /usr/lib/wsl/lib ]; then
            printf "/usr/lib/wsl/lib\n/usr/local/cuda/lib64\n" | sudo tee /etc/ld.so.conf.d/wsl-nvidia.conf >/dev/null
            sudo ldconfig 2>/dev/null || true
        fi
    fi
fi

detect_nvcc() {
    for c in /usr/local/cuda-13.3/bin/nvcc /usr/local/cuda-13.2/bin/nvcc /usr/local/cuda-13.1/bin/nvcc \
             /usr/local/cuda-13.0/bin/nvcc /usr/local/cuda/bin/nvcc /usr/bin/nvcc; do
        if [ -x "${c}" ]; then
            echo "${c}"
            return 0
        fi
    done
    if command -v nvcc >/dev/null 2>&1; then
        command -v nvcc
        return 0
    fi
    return 1
}

NVCC_BIN="$(detect_nvcc || true)"
if [ -z "${NVCC_BIN}" ] && [ "${LAIS_ONLY}" -eq 0 ] && command -v sudo >/dev/null 2>&1 && [ -n "${CUDA_KEYRING_URL}" ]; then
    echo -e "\033[1;33m[INFO]\033[0m NVIDIA CUDA Toolkit not found. Installing ${CUDA_APT_PKG} from ${SOURCES_JSON}..."
    wget -q "${CUDA_KEYRING_URL}" -O /tmp/cuda-keyring.deb
    sudo dpkg -i /tmp/cuda-keyring.deb && rm -f /tmp/cuda-keyring.deb
    sudo DEBIAN_FRONTEND=noninteractive apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${CUDA_APT_PKG}"
    NVCC_BIN="$(detect_nvcc || true)"
fi

CUDA_CMAKE_FLAGS="-DCMAKE_C_FLAGS=-w -DCMAKE_CXX_FLAGS=-w"
HAS_CUDA=0

if [ -n "${NVCC_BIN}" ] && [ -x "${NVCC_BIN}" ]; then
    HAS_CUDA=1
    CUDA_ROOT="$(cd "$(dirname "${NVCC_BIN}")/.." && pwd)"
    export PATH="$(dirname "${NVCC_BIN}"):/usr/lib/wsl/lib:${PATH}"
    export LD_LIBRARY_PATH="/usr/lib/wsl/lib:${CUDA_ROOT}/lib64:${LD_LIBRARY_PATH:-}"
    export CUDA_HOME="${CUDA_ROOT}"

    SYS_INC="${CUDA_ROOT}/targets/x86_64-linux/include"
    [ -d "${SYS_INC}" ] || SYS_INC="${CUDA_ROOT}/include"
    if [ -f "${SYS_INC}/crt/math_functions.h" ]; then
        cp -f "${SYS_INC}/cuda_runtime.h" "${COMPAT_DIR}/cuda_runtime.h" 2>/dev/null || true
        cp -f "${SYS_INC}/crt/"* "${COMPAT_DIR}/crt/" 2>/dev/null || true
        python3 - "${SYS_INC}/crt/math_functions.h" "${COMPAT_DIR}/crt/math_functions.h" << 'PYEOF'
import sys
from pathlib import Path

src_path = Path(sys.argv[1])
dst_path = Path(sys.argv[2])
txt = src_path.read_text(encoding="utf-8", errors="ignore")
txt = txt.replace(" noexcept (true)", "").replace(" noexcept(true)", "")

targets = [
    ("rsqrt(double x);", "rsqrt(double x) noexcept (true);"),
    ("rsqrtf(float x);", "rsqrtf(float x) noexcept (true);"),
    ("rsqrt(double a));", "rsqrt(double a) noexcept (true));"),
    ("rsqrtf(float a));", "rsqrtf(float a) noexcept (true));"),
    ("sinpi(double x);", "sinpi(double x) noexcept (true);"),
    ("sinpif(float x);", "sinpif(float x) noexcept (true);"),
    ("cospi(double x);", "cospi(double x) noexcept (true);"),
    ("cospif(float x);", "cospif(float x) noexcept (true);"),
    ("sinpi(double a));", "sinpi(double a) noexcept (true));"),
    ("sinpif(float a));", "sinpif(float a) noexcept (true));"),
    ("cospi(double a));", "cospi(double a) noexcept (true));"),
    ("cospif(float a));", "cospif(float a) noexcept (true));"),
]
for old, new in targets:
    pos = 0
    while True:
        idx = txt.find(old, pos)
        if idx == -1:
            break
        if idx == 0 or txt[idx - 1].isspace() or txt[idx - 1] == "(":
            txt = txt[:idx] + new + txt[idx + len(old):]
            pos = idx + len(new)
        else:
            pos = idx + len(old)

dst_path.write_text(txt, encoding="utf-8")
PYEOF
        if command -v sudo >/dev/null 2>&1; then
            sudo cp -f "${COMPAT_DIR}/crt/math_functions.h" "${SYS_INC}/crt/math_functions.h" 2>/dev/null || true
            sudo chmod 0644 "${SYS_INC}/crt/math_functions.h" 2>/dev/null || true
        fi
    fi

    HOST_CC=""
    HOST_CXX=""
    if [ -x /usr/bin/gcc-14 ] && [ -x /usr/bin/g++-14 ]; then
        HOST_CC="/usr/bin/gcc-14"
        HOST_CXX="/usr/bin/g++-14"
    elif [ -x /usr/bin/gcc-13 ] && [ -x /usr/bin/g++-13 ]; then
        HOST_CC="/usr/bin/gcc-13"
        HOST_CXX="/usr/bin/g++-13"
    fi

    if [ -n "${HOST_CXX}" ]; then
        export CC="${HOST_CC}"
        export CXX="${HOST_CXX}"
        export CUDAHOSTCXX="${HOST_CXX}"
        export NVCC_CCBIN="${HOST_CXX}"
        CUDA_CMAKE_FLAGS="${CUDA_CMAKE_FLAGS} -DCMAKE_C_COMPILER=${HOST_CC} -DCMAKE_CXX_COMPILER=${HOST_CXX} -DCMAKE_CUDA_HOST_COMPILER=${HOST_CXX}"
    fi

    export NVCC_PREPEND_FLAGS="-allow-unsupported-compiler -w -I${COMPAT_DIR}"
    export NVCC_APPEND_FLAGS="-w -Xcompiler -w,-Wno-unused-parameter,-Wno-unused-variable,-Wno-sign-compare,-Wno-deprecated-declarations"
    export NVCC_THREADS=2

    GPU_ARCH="$(PATH="/usr/lib/wsl/lib:${PATH}" nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -n 1 | tr -dc '0-9' || true)"
    [ -n "${GPU_ARCH}" ] || GPU_ARCH="native"

    CUDA_CMAKE_FLAGS="${CUDA_CMAKE_FLAGS} -DCMAKE_CUDA_COMPILER=${NVCC_BIN} -DCMAKE_CUDA_ARCHITECTURES=${GPU_ARCH} -DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler"
    echo -e "\033[1;32m[2/5]\033[0m CUDA Toolchain Ready: ${NVCC_BIN} (Host C++: ${HOST_CXX:-default}, GPU Arch: ${GPU_ARCH})"
else
    echo -e "\033[1;33m[2/5]\033[0m CUDA nvcc not detected (or --lais-only set); using CPU/existing engine mode."
fi

restore_clean_cmake() {
    local repo="$1"
    if [ -d "${repo}/.git" ]; then
        git -C "${repo}" checkout -- . 2>/dev/null || true
        git -C "${repo}" submodule foreach --recursive 'git checkout -- .' 2>/dev/null || true
    fi
    for f in "${repo}/CMakeLists.txt" "${repo}/ggml/CMakeLists.txt" "${repo}/ggml/src/CMakeLists.txt" "${repo}/ggml/src/ggml-cuda/CMakeLists.txt"; do
        if [ -f "${f}" ]; then
            sed -i -E 's/(^|[[:space:]";])=implicit-/\1-Werror=implicit-/g; s/(^|[[:space:]";])=vla/\1-Werror=vla/g' "${f}" 2>/dev/null || true
        fi
    done
}

ensure_llama_cpp_min_build() {
    local repo="$1"
    local min_build="${LLAMA_MIN_BUILD}"
    if [ ! -d "${repo}/.git" ]; then
        return 1
    fi
    git -C "${repo}" fetch --tags --depth 1 origin master 2>/dev/null || true
    local latest_tag
    latest_tag="$(git -C "${repo}" tag -l 'b[0-9]*' | sed 's/^b//' | sort -n | tail -n 1 || true)"
    if [ -n "${latest_tag}" ] && [ "${latest_tag}" -lt "${min_build}" ] 2>/dev/null; then
        git -C "${repo}" pull --ff-only origin master 2>/dev/null || git -C "${repo}" reset --hard origin/master
        return 1
    fi
    if [ -x "${repo}/build/bin/llama-server" ]; then
        local ver_out
        ver_out="$(LD_LIBRARY_PATH="${repo}/build/bin:/usr/lib/wsl/lib:${LD_LIBRARY_PATH:-}" "${repo}/build/bin/llama-server" --version 2>&1 || true)"
        local built_num
        built_num="$(echo "${ver_out}" | grep -oE 'version: [0-9]+' | awk '{print $2}' | head -n 1 || true)"
        if [ -n "${built_num}" ] && [ "${built_num}" -gt 0 ] && [ "${built_num}" -lt "${min_build}" ] 2>/dev/null; then
            git -C "${repo}" fetch --unshallow origin master 2>/dev/null || git -C "${repo}" fetch origin master
            git -C "${repo}" reset --hard origin/master
            return 1
        fi
    fi
    return 0
}

HW_THREADS="$(nproc 2>/dev/null || echo 4)"
BUILD_JOBS=$(( HW_THREADS > 8 ? 8 : HW_THREADS ))

if [ "${LAIS_ONLY}" -eq 0 ]; then
    echo -e "\033[1;34m[3/5]\033[0m Building C++ AI Inference Engines in ${ENGINES_DIR} (Jobs: ${BUILD_JOBS})..."

    LLAMA_DIR="${ENGINES_DIR}/llama.cpp"
    restore_clean_cmake "${LLAMA_DIR}"
    NEED_LLAMA_BUILD=0
    if [ ! -x "${LLAMA_DIR}/build/bin/llama-server" ] || [ "${FORCE_REBUILD}" -eq 1 ]; then
        NEED_LLAMA_BUILD=1
    elif ! ensure_llama_cpp_min_build "${LLAMA_DIR}"; then
        NEED_LLAMA_BUILD=1
    fi

    if [ "${NEED_LLAMA_BUILD}" -eq 1 ]; then
        echo -e "\033[1;34m  ->\033[0m Building llama.cpp (>= b${LLAMA_MIN_BUILD}, source: ${LLAMA_REPO})..."
        if [ ! -d "${LLAMA_DIR}/.git" ]; then
            rm -rf "${LLAMA_DIR}"
            git clone "${LLAMA_REPO}" "${LLAMA_DIR}"
        else
            git -C "${LLAMA_DIR}" fetch --unshallow origin master 2>/dev/null || git -C "${LLAMA_DIR}" fetch origin master
            git -C "${LLAMA_DIR}" reset --hard origin/master
        fi
        restore_clean_cmake "${LLAMA_DIR}"
        rm -rf "${LLAMA_DIR}/build/CMakeCache.txt" "${LLAMA_DIR}/build/CMakeFiles"
        GGML_CUDA_OPT="$( [ "${HAS_CUDA}" -eq 1 ] && echo "-DGGML_CUDA=ON" || echo "-DGGML_CUDA=OFF" )"
        # shellcheck disable=SC2086
        cmake -S "${LLAMA_DIR}" -B "${LLAMA_DIR}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            ${GGML_CUDA_OPT} -DBUILD_SHARED_LIBS=OFF \
            -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON \
            ${CUDA_CMAKE_FLAGS}
        cmake --build "${LLAMA_DIR}/build" --config Release --target llama-server llama-cli -j"${BUILD_JOBS}"
    else
        echo -e "\033[1;32m  ->\033[0m Reusing verified llama.cpp >= b${LLAMA_MIN_BUILD}"
    fi

    SD_DIR="${ENGINES_DIR}/stable-diffusion.cpp"
    restore_clean_cmake "${SD_DIR}"
    if { [ ! -x "${SD_DIR}/build/bin/sd-cli" ] && [ ! -x "${SD_DIR}/build/bin/sd" ]; } || [ "${FORCE_REBUILD}" -eq 1 ]; then
        echo -e "\033[1;34m  ->\033[0m Building stable-diffusion.cpp (Release ${SD_VERIFIED_TAG})..."
        if [ ! -d "${SD_DIR}/.git" ]; then
            rm -rf "${SD_DIR}"
            git clone --depth 1 --branch "${SD_VERIFIED_TAG}" --recurse-submodules \
                "${SD_REPO}" "${SD_DIR}" || \
            git clone --depth 1 --recurse-submodules "${SD_REPO}" "${SD_DIR}"
        fi
        if [ "${FORCE_REBUILD}" -eq 1 ] || [ -f "${SD_DIR}/ggml/src/ggml-cuda/mul-mat-i8.cu" ]; then
            git -C "${SD_DIR}" fetch --depth 1 origin tag "${SD_VERIFIED_TAG}" && \
            git -C "${SD_DIR}" checkout -f "${SD_VERIFIED_TAG}" && \
            git -C "${SD_DIR}" submodule update --init --recursive --force && \
            git -C "${SD_DIR}/ggml" clean -fdx
        else
            git -C "${SD_DIR}" submodule update --init --recursive
        fi

        restore_clean_cmake "${SD_DIR}"
        rm -rf "${SD_DIR}/build/CMakeCache.txt" "${SD_DIR}/build/CMakeFiles"
        SD_CUDA_OPT="$( [ "${HAS_CUDA}" -eq 1 ] && echo "-DSD_CUDA=ON" || echo "-DSD_CUDA=OFF" )"

        # shellcheck disable=SC2086
        if ! (cmake -S "${SD_DIR}" -B "${SD_DIR}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
                ${SD_CUDA_OPT} -DSD_BUILD_SHARED_LIBS=OFF ${CUDA_CMAKE_FLAGS} && \
              cmake --build "${SD_DIR}/build" --config Release -j"${BUILD_JOBS}"); then
            rm -rf "${SD_DIR}/build/CMakeCache.txt" "${SD_DIR}/build/CMakeFiles"
            # shellcheck disable=SC2086
            cmake -S "${SD_DIR}" -B "${SD_DIR}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
                ${SD_CUDA_OPT} -DSD_BUILD_SHARED_LIBS=OFF -DSD_USE_UPSTREAM_GGML=ON \
                -DSD_GGML_SOURCE_DIR="${LLAMA_DIR}/ggml" ${CUDA_CMAKE_FLAGS}
            cmake --build "${SD_DIR}/build" --config Release -j"${BUILD_JOBS}"
        fi
    else
        echo -e "\033[1;32m  ->\033[0m Reusing already-built stable-diffusion.cpp"
    fi

    ACE_DIR="${ENGINES_DIR}/acestep.cpp"
    restore_clean_cmake "${ACE_DIR}"
    if { [ ! -x "${ACE_DIR}/build/ace-synth" ] && [ ! -x "${ACE_DIR}/build/bin/ace-synth" ]; } || [ "${FORCE_REBUILD}" -eq 1 ]; then
        echo -e "\033[1;34m  ->\033[0m Building acestep.cpp..."
        if [ ! -d "${ACE_DIR}/.git" ]; then
            rm -rf "${ACE_DIR}"
            git clone --depth 1 --recurse-submodules "${ACE_REPO}" "${ACE_DIR}"
        else
            if [ "${FORCE_REBUILD}" -eq 1 ]; then
                git -C "${ACE_DIR}" fetch --depth 1 origin master
                git -C "${ACE_DIR}" checkout --detach FETCH_HEAD
            fi
            git -C "${ACE_DIR}" submodule update --init --recursive
        fi
        restore_clean_cmake "${ACE_DIR}"
        rm -rf "${ACE_DIR}/build/CMakeCache.txt" "${ACE_DIR}/build/CMakeFiles"
        ACE_CUDA_OPT="$( [ "${HAS_CUDA}" -eq 1 ] && echo "-DGGML_CUDA=ON" || echo "-DGGML_CUDA=OFF" )"
        # shellcheck disable=SC2086
        cmake -S "${ACE_DIR}" -B "${ACE_DIR}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            ${ACE_CUDA_OPT} -DBUILD_SHARED_LIBS=OFF ${CUDA_CMAKE_FLAGS}
        cmake --build "${ACE_DIR}/build" --config Release -j"${BUILD_JOBS}"
    else
        echo -e "\033[1;32m  ->\033[0m Reusing already-built acestep.cpp"
    fi
fi

LLAMA_DIR="${ENGINES_DIR}/llama.cpp"
SD_DIR="${ENGINES_DIR}/stable-diffusion.cpp"
ACE_DIR="${ENGINES_DIR}/acestep.cpp"
for f in "${LLAMA_DIR}/build/bin/llama-server" "${LLAMA_DIR}/build/bin/llama-cli" \
         "${SD_DIR}/build/bin/sd-cli" "${SD_DIR}/build/bin/sd" \
         "${ACE_DIR}/build/ace-lm" "${ACE_DIR}/build/bin/ace-lm" \
         "${ACE_DIR}/build/ace-synth" "${ACE_DIR}/build/bin/ace-synth"; do
    if [ -f "${f}" ]; then
        safe_copy_file "${f}" "${BIN_DIR}/$(basename "${f}")" "overwrite"
        strip -s "${BIN_DIR}/$(basename "${f}")" 2>/dev/null || true
        chmod 0755 "${BIN_DIR}/$(basename "${f}")"
    fi
done
find "${LLAMA_DIR}/build" "${SD_DIR}/build" "${ACE_DIR}/build" -maxdepth 3 -name "*.so*" -exec cp -P -f {} "${BIN_DIR}/" \; 2>/dev/null || true

echo -e "\033[1;34m[4/5]\033[0m Compiling modular C++20 translation units (src/*.cpp) in parallel into build/lais..."

RAW_BIN="${BUILD_OUT_DIR}/lais.raw"
OUT_BIN="${BUILD_OUT_DIR}/lais"

CXX_COMPILER="g++"
if [ -x /usr/bin/g++-14 ]; then
    CXX_COMPILER="/usr/bin/g++-14"
fi

MODULES=(common telemetry crypto_pake coordinator http_server engine_manager main line_editor)
for mod in "${MODULES[@]}"; do
    "${CXX_COMPILER}" -std=c++20 -O3 -DNDEBUG -fno-plt \
        -Wall -Wextra -Wno-unused-result -Wno-restrict -I"${SCRIPT_DIR}/src" -pthread \
        -c "${SCRIPT_DIR}/src/${mod}.cpp" -o "${OBJ_DIR}/${mod}.o" &
done
wait

OBJ_FILES=()
for mod in "${MODULES[@]}"; do
    OBJ_FILES+=("${OBJ_DIR}/${mod}.o")
done

"${CXX_COMPILER}" -std=c++20 -O3 -DNDEBUG -pthread "${OBJ_FILES[@]}" -o "${RAW_BIN}"
strip -s "${RAW_BIN}" 2>/dev/null || true
chmod 0755 "${RAW_BIN}"

BUNDLE_TAR="${CACHE_DIR}/tmp/engines_bundle.tar.gz"
rm -f "${BUNDLE_TAR}"

python3 - "${RAW_BIN}" "${OUT_BIN}" "${BIN_DIR}" "${BUNDLE_TAR}" << 'PYEOF'
import os
import shutil
import subprocess
import sys
from pathlib import Path

raw_bin = Path(sys.argv[1])
out_bin = Path(sys.argv[2])
bin_dir = Path(sys.argv[3])
bundle_tar = Path(sys.argv[4])

bundle_entries = []
for known in ["llama-server", "llama-cli", "sd-cli", "sd", "ace-lm", "ace-synth", "configs", "data"]:
    if (bin_dir / known).exists():
        bundle_entries.append(known)
if bin_dir.exists():
    try:
        for p in sorted(bin_dir.iterdir()):
            if p.name not in ("lais", ".lais_bundle_stamp") and p.name not in bundle_entries:
                bundle_entries.append(p.name)
    except Exception:
        pass

shutil.copy2(raw_bin, out_bin)

if bundle_entries:
    subprocess.run(
        ["tar", "-czf", str(bundle_tar), "-C", str(bin_dir)] + bundle_entries,
        check=True
    )
    offset = out_bin.stat().st_size
    payload = bundle_tar.read_bytes()
    length = len(payload)
    trailer = f"\nLAIS_BUNDLE_V1|{offset}|{length}|END_LAIS_BUNDLE\n".encode("ascii")
    with out_bin.open("ab") as f:
        f.write(payload)
        f.write(trailer)
    bundle_tar.unlink(missing_ok=True)
    print(f"  -> Embedded {len(bundle_entries)} bundle item(s) ({length / (1024*1024):.2f} MB compressed) into build/{out_bin.name}")
else:
    print(f"  -> Built standalone build/{out_bin.name}")

os.chmod(out_bin, 0o755)
raw_bin.unlink(missing_ok=True)
PYEOF

mkdir -p "${HOME}/.local/bin" "${BIN_DIR}"
safe_copy_file "${OUT_BIN}" "${HOME}/.local/bin/lais" "overwrite"
safe_copy_file "${OUT_BIN}" "${BIN_DIR}/lais" "overwrite"
chmod 0755 "${HOME}/.local/bin/lais" "${BIN_DIR}/lais"
if command -v sudo >/dev/null 2>&1; then
    sudo cp -f "${OUT_BIN}" /usr/local/bin/lais 2>/dev/null && sudo chmod 0755 /usr/local/bin/lais 2>/dev/null || true
fi

if pgrep -u "$(id -u)" -f "[l]ais --serve" >/dev/null 2>&1; then
    echo -e "\033[1;34m[INFO]\033[0m Restarting active LAIS background service with updated binary..."
    "${OUT_BIN}" restart || true
fi

BASHRC="${HOME}/.bashrc"
touch "${BASHRC}"
if ! grep -q "# >>> LAIS PATH >>>" "${BASHRC}" 2>/dev/null; then
    cat >> "${BASHRC}" << 'EOF'
# >>> LAIS PATH >>>
export PATH="$HOME/.local/bin:$HOME/lais/bin:/usr/local/cuda/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/lais/bin:/usr/lib/wsl/lib:/usr/local/cuda/lib64:${LD_LIBRARY_PATH:-}"
# <<< LAIS PATH <<<
EOF
fi

BUILD_ZIP="${BUILD_OUT_DIR}/lais.zip"
ROOT_ZIP="${SCRIPT_DIR}/lais.zip"
HOME_BACKUP_ZIP="${LAIS_DIR}/lais.zip"
echo -e "\033[1;34m[5/5]\033[0m Compressing built program, configs, and modular sources into ${BUILD_ZIP} ..."

python3 - "${SCRIPT_DIR}" "${BIN_DIR}" "${OUT_BIN}" "${BUILD_ZIP}" << 'PYEOF'
import sys
import zipfile
from pathlib import Path

script_dir = Path(sys.argv[1])
bin_dir = Path(sys.argv[2])
out_bin = Path(sys.argv[3])
backup_zip = Path(sys.argv[4])

restore_sh = """#!/usr/bin/env bash
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
"""

with zipfile.ZipFile(backup_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    if out_bin.exists():
        zf.write(out_bin, arcname="lais/build/lais")

    for eng in ["llama-server", "llama-cli", "sd-cli", "sd", "ace-lm", "ace-synth"]:
        ep = bin_dir / eng
        if ep.exists() and ep.is_file():
            zf.write(ep, arcname=f"lais/bin/{eng}")

    for cfg_name in ["settings.json", "sources.json", "models.json", "en-us.lang", "ko-kr.lang"]:
        cp = script_dir / "configs" / cfg_name
        if cp.exists():
            zf.write(cp, arcname=f"lais/configs/{cfg_name}")

    up = script_dir / "data" / "user.json"
    if up.exists():
        zf.write(up, arcname="lais/data/user.json")

    for src_file in [
        "common.hpp", "common.cpp", "crypto_pake.hpp", "crypto_pake.cpp",
        "telemetry.cpp", "coordinator.cpp", "webui.hpp", "http_server.cpp",
        "engine_manager.cpp", "main.cpp", "line_editor.cpp", "line_editor.hpp"
    ]:
        sp = script_dir / "src" / src_file
        if sp.exists():
            zf.write(sp, arcname=f"lais/src/{src_file}")

    for root_file in ["build.sh", "build.bat", "CMakeLists.txt", "README.md", "MODEL_UPDATE_KO.md", "LICENSE", ".gitignore"]:
        rp = script_dir / root_file
        if rp.exists():
            zf.write(rp, arcname=f"lais/{root_file}")

    for directory in ["tools", "tests"]:
        for extra in (script_dir / directory).glob("*"):
            if extra.is_file():
                zf.write(extra, arcname=f"lais/{directory}/{extra.name}")

    info = zipfile.ZipInfo("lais/restore.sh")
    info.external_attr = 0o755 << 16
    zf.writestr(info, restore_sh)

print(f"  -> Created backup archive: {backup_zip} ({backup_zip.stat().st_size / (1024*1024):.2f} MB)")
PYEOF

safe_copy_file "${BUILD_ZIP}" "${ROOT_ZIP}" "overwrite"
safe_copy_file "${BUILD_ZIP}" "${HOME_BACKUP_ZIP}" "overwrite"

echo -e "\033[1;36m==============================================================================\033[0m"
echo -e "\033[1;32m[BUILD & BACKUP COMPLETE]\033[0m"
echo -e "  * Final Binary    : ${OUT_BIN} (also installed at /usr/local/bin/lais)"
echo -e "  * JSON Configs    : ${CONFIGS_DIR}/{settings.json,sources.json,models.json}"
echo -e "  * PAKE User Store : ${DATA_DIR}/user.json (Initial ID/PW: admin / admin)"
echo -e "  * Backup ZIP File : ${BUILD_ZIP}"
echo -e "                      ${ROOT_ZIP}"
echo -e "\033[1;36m==============================================================================\033[0m"
