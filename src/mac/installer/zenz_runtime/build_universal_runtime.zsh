#!/bin/zsh

set -eu
setopt pipe_fail

export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:${PATH:-}"

LLAMA_REPOSITORY="https://github.com/ggml-org/llama.cpp.git"
LLAMA_TAG="v0.7.0"
LLAMA_COMMIT="620d2225cc436ab0b001e3200e590e3b286ef9ab"
PATCH_SHA256="6db5c11b2da8415d6b37200ec6aa4f3fdcdde8efb1e7a71375fe331a7b0e829a"
PATCHED_SOURCE_SHA256="3a65249edbcaf4a21742e44f89a2be810a75e53c892bb5905e1a5dc71227184d"
DUAL_LIVENESS_PATCH_SHA256="3267e978f61456c32dfb83b15bf0f3a75393c5f98c946ee7a2846f7121b36dda"
DUAL_LIVENESS_MAIN_SHA256="2197ffc83775b865b97b35528a99f1daa692cf25eb4e952509e46e2cfebbf976"

MODEL_REPOSITORY="Miwa-Keita/zenz-v3.2-small-gguf"
MODEL_REVISION="c67e03e07d215c869f591b274c1631170d3e11fe"
MODEL_REMOTE_FILE="ggml-model-Q5_K_M.gguf"
MODEL_OUTPUT_FILE="zenz-v3.2-small-Q5_K_M.gguf"
MODEL_SHA256="29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673"
MODEL_URL="https://huggingface.co/${MODEL_REPOSITORY}/resolve/${MODEL_REVISION}/${MODEL_REMOTE_FILE}?download=true"

MINIMUM_MACOS="12.0"
CONTRACT_FORMAT="mozkey-macos-zenz-runtime-v1"

die() {
  print -u2 -- "ERROR: $*"
  exit 1
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" |
    /usr/bin/awk '{print $1}'
}

extract_minos() {
  /usr/bin/otool -l "$1" |
    /usr/bin/awk '
      /LC_BUILD_VERSION/ {
        found = 1
        next
      }

      found && $1 == "minos" {
        print $2
        exit
      }
    '
}

check_system_only() {
  local binary="$1"
  local unexpected

  unexpected="$(
    /usr/bin/otool -L "$binary" |
      /usr/bin/awk 'NR > 1 {print $1}' |
      /usr/bin/grep -Ev '^(/System/Library/|/usr/lib/)' ||
      true
  )"

  [[ -z "$unexpected" ]] || {
    print -u2 -- "Unexpected non-system dynamic libraries in $binary:"
    print -u2 -- "$unexpected"
    return 1
  }
}

check_tokenizer_marker() {
  /usr/bin/strings "$1" |
    /usr/bin/grep -F \
      'gpt2-small-japanese-char' \
      >/dev/null
}

audit_thin() {
  local binary="$1"
  local expected_arch="$2"

  [[ -f "$binary" ]] ||
    die "Missing $expected_arch llama-server: $binary"

  local archs
  archs="$(/usr/bin/lipo -archs "$binary")"

  [[ "$archs" == "$expected_arch" ]] ||
    die "Expected $expected_arch thin binary, got: $archs"

  local minos
  minos="$(extract_minos "$binary")"

  [[ "$minos" == "$MINIMUM_MACOS" ]] ||
    die "$expected_arch minimum macOS mismatch: $minos"

  check_system_only "$binary" ||
    die "$expected_arch dynamic-library contract failed"

  check_tokenizer_marker "$binary" ||
    die "$expected_arch tokenizer compatibility marker missing"
}

sanitize_contract_value() {
  /usr/bin/tr '\n\r=' '   ' |
    /usr/bin/sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//'
}

output_root="${1:-}"

if [[ -z "$output_root" ]]; then
  print -u2 -- "Usage:"
  print -u2 -- "  $0 /path/to/output-runtime"
  print -u2 -- ""
  print -u2 -- "Optional:"
  print -u2 -- "  MOZKEY_ZENZ_MODEL_SOURCE=/path/to/zenz-v3.2-small-Q5_K_M.gguf"
  exit 2
fi

output_root="${output_root:A}"

if [[ -e "$output_root" ]]; then
  if [[ ! -d "$output_root" ]]; then
    die "Output path exists and is not a directory: $output_root"
  fi

  if [[ -n "$(/bin/ls -A "$output_root" 2>/dev/null)" ]]; then
    die "Output directory must be empty: $output_root"
  fi
else
  /bin/mkdir -p "$output_root"
fi

script_path="${0:A}"
script_dir="${script_path:h}"
patch_file="$script_dir/patches/llama-vocab-gpt2-small-japanese-char.patch"
dual_patch_file="$script_dir/patches/llama-server-mozkey-dual-liveness.patch"

[[ -f "$patch_file" ]] ||
  die "Tracked patch missing: $patch_file"

actual_patch_sha="$(sha256_file "$patch_file")"

[[ "$actual_patch_sha" == "$PATCH_SHA256" ]] ||
  die "Tracked patch SHA-256 mismatch"
[[ -f "$dual_patch_file" ]] || die "Missing dual-liveness patch"
[[ "$(sha256_file "$dual_patch_file")" == "$DUAL_LIVENESS_PATCH_SHA256" ]] ||
  die "Dual-liveness patch SHA256 mismatch"

host_arch="$(/usr/bin/uname -m)"

[[ "$host_arch" == "arm64" ]] ||
  die "Formal Universal runtime build currently requires an Apple Silicon host"

required_commands=(
  git
  cmake
  curl
)

for command_name in "${required_commands[@]}"; do
  command -v "$command_name" >/dev/null ||
    die "Required command not found: $command_name"
done

required_tools=(
  /usr/bin/shasum
  /usr/bin/awk
  /usr/bin/file
  /usr/bin/lipo
  /usr/bin/otool
  /usr/bin/strings
  /usr/bin/grep
  /usr/bin/codesign
  /usr/bin/xcodebuild
  /usr/bin/sw_vers
  /usr/bin/install
  /usr/bin/mktemp
  /usr/sbin/sysctl
  /bin/mkdir
  /bin/rm
  /bin/cp
)

for tool_file in "${required_tools[@]}"; do
  [[ -x "$tool_file" ]] ||
    die "Required tool missing: $tool_file"
done

logs="$output_root/logs"
models="$output_root/models"
universal="$output_root/llama-server"
output_model="$models/$MODEL_OUTPUT_FILE"
contract="$output_root/BUILD-CONTRACT.txt"

work_root="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/mozkey-zenz-builder.XXXXXX")"
source_dir="$work_root/source"
arm_build="$work_root/build-arm64"
x86_build="$work_root/build-x86_64"
toolchain="$work_root/x86_64-macos12-toolchain.cmake"
smoke_runtime="$work_root/llama-server-smoke"

cleanup() {
  /bin/rm -rf "$work_root"
}

trap cleanup EXIT

/bin/mkdir -p "$logs" "$models"

echo "===== FORMAL RUNTIME BUILDER PRECHECK ====="
echo "Output root      = $output_root"
echo "Host arch        = $host_arch"
echo "llama.cpp tag    = $LLAMA_TAG"
echo "llama.cpp commit = $LLAMA_COMMIT"
echo "Patch SHA256     = $actual_patch_sha"
echo "Model revision   = $MODEL_REVISION"

echo
echo "===== CLONE EXACT llama.cpp SOURCE ====="

git clone \
  --quiet \
  --depth 1 \
  --branch "$LLAMA_TAG" \
  "$LLAMA_REPOSITORY" \
  "$source_dir"

actual_commit="$(git -C "$source_dir" rev-parse HEAD)"

echo "Actual commit = $actual_commit"

[[ "$actual_commit" == "$LLAMA_COMMIT" ]] ||
  die "Unexpected llama.cpp commit"

[[ -z "$(git -C "$source_dir" status --porcelain)" ]] ||
  die "Fresh llama.cpp checkout is unexpectedly dirty"

echo
echo "===== CONFIGURE BOTH ARCHITECTURES WHILE SOURCE IS CLEAN ====="

cat > "$toolchain" <<CMAKE
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "/usr/bin/cc")
set(CMAKE_CXX_COMPILER "/usr/bin/c++")

set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "$MINIMUM_MACOS" CACHE STRING "" FORCE)
CMAKE

COMMON_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF
  -DGGML_ACCELERATE=ON
  -DGGML_BLAS=OFF
  -DGGML_CCACHE=OFF
  -DGGML_CPU_KLEIDIAI=OFF
  -DGGML_METAL=ON
  -DGGML_METAL_EMBED_LIBRARY=ON
  -DGGML_NATIVE=OFF
  -DGGML_OPENMP=OFF
  -DLLAMA_BUILD_COMMON=ON
  -DLLAMA_BUILD_EXAMPLES=OFF
  -DLLAMA_BUILD_MTMD=OFF
  -DLLAMA_BUILD_SERVER=ON
  -DLLAMA_BUILD_TESTS=OFF
  -DLLAMA_BUILD_TOOLS=ON
  -DLLAMA_BUILD_UI=OFF
  -DLLAMA_LLGUIDANCE=OFF
  -DLLAMA_OPENSSL=OFF
  -DLLAMA_SUBPROCESS=ON
  -DLLAMA_USE_PREBUILT_UI=OFF
  -DLLAMA_USE_SYSTEM_GGML=OFF
)

cmake \
  -S "$source_dir" \
  -B "$arm_build" \
  "${COMMON_ARGS[@]}" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MINIMUM_MACOS" \
  2>&1 |
  /usr/bin/tee "$logs/configure-arm64.log"

cmake \
  -S "$source_dir" \
  -B "$x86_build" \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  "${COMMON_ARGS[@]}" \
  2>&1 |
  /usr/bin/tee "$logs/configure-x86_64.log"

# llama.cpp v0.7.0 provisions ui.cpp/ui.h with CMake at build time;
# the former native host-helper path no longer exists. Keep UI provisioning
# disabled and offline in both architecture-specific CMake caches.
for build_dir in "$arm_build" "$x86_build"; do
  cache="$build_dir/CMakeCache.txt"
  [[ -f "$cache" ]] || die "Missing CMake cache: $cache"
  for option in LLAMA_BUILD_UI LLAMA_USE_PREBUILT_UI; do
    /usr/bin/grep -Eq "^${option}:(BOOL|UNINITIALIZED)=OFF$" "$cache" ||
      die "Offline UI option is not OFF: $option in $build_dir"
  done
done

/usr/bin/grep -F \
  'LLAMA_COMMIT = "620d222"' \
  "$arm_build/common/build-info.cpp" \
  >/dev/null ||
  die "arm64 build-info commit mismatch"

/usr/bin/grep -F \
  'LLAMA_BUILD_TARGET = "Darwin arm64"' \
  "$arm_build/common/build-info.cpp" \
  >/dev/null ||
  die "arm64 build-info target mismatch"

/usr/bin/grep -F \
  'LLAMA_COMMIT = "620d222"' \
  "$x86_build/common/build-info.cpp" \
  >/dev/null ||
  die "x86_64 build-info commit mismatch"

/usr/bin/grep -F \
  'LLAMA_BUILD_TARGET = "Darwin x86_64"' \
  "$x86_build/common/build-info.cpp" \
  >/dev/null ||
  die "x86_64 build-info target mismatch"

echo
echo "===== APPLY TRACKED ZENZ PATCH ====="

git -C "$source_dir" apply --check "$patch_file"
git -C "$source_dir" apply "$patch_file"

patched_source_sha="$(sha256_file "$source_dir/src/llama-vocab.cpp")"

echo "Patched llama-vocab.cpp SHA256 = $patched_source_sha"

[[ "$patched_source_sha" == "$PATCHED_SOURCE_SHA256" ]] ||
  die "Patched llama-vocab.cpp identity mismatch"

# Keep the existing tokenizer patch intact; apply a second independent patch
# against the pinned upstream server entrypoint only.
git -C "$source_dir" apply --check "$dual_patch_file"
git -C "$source_dir" apply "$dual_patch_file"
[[ "$(sha256_file "$source_dir/tools/server/main.cpp")" == "$DUAL_LIVENESS_MAIN_SHA256" ]] ||
  die "Patched llama-server main.cpp identity mismatch"

echo
echo "===== BUILD BOTH THIN RUNTIMES ====="

cpu_count="$(/usr/sbin/sysctl -n hw.logicalcpu)"

cmake \
  --build "$arm_build" \
  --config Release \
  --target llama-server \
  --parallel "$cpu_count" \
  2>&1 |
  /usr/bin/tee "$logs/build-arm64.log"

cmake \
  --build "$x86_build" \
  --config Release \
  --target llama-server \
  --parallel "$cpu_count" \
  2>&1 |
  /usr/bin/tee "$logs/build-x86_64.log"

arm="$arm_build/bin/llama-server"
x86="$x86_build/bin/llama-server"
# The server's llama-ui target depends on CMake's generated UI sources.
# Verify generation independently for both target architectures before lipo.
for build_dir in "$arm_build" "$x86_build"; do
  for generated in ui.cpp ui.h; do
    [[ -s "$build_dir/tools/ui/$generated" ]] ||
      die "Missing CMake-generated $generated in $build_dir"
  done
done

audit_thin "$arm" arm64
audit_thin "$x86" x86_64

arm_sha="$(sha256_file "$arm")"
x86_sha="$(sha256_file "$x86")"

echo "Generated arm64 SHA256  = $arm_sha"
echo "Generated x86_64 SHA256 = $x86_sha"

echo
echo "===== CREATE UNSIGNED UNIVERSAL RUNTIME ====="

/usr/bin/lipo \
  -create \
  "$arm" \
  "$x86" \
  -output "$universal"

/bin/chmod 0755 "$universal"

/usr/bin/xattr -c "$universal" 2>/dev/null || true

archs="$(/usr/bin/lipo -archs "$universal")"

print -- "$archs" |
  /usr/bin/grep -qw arm64 ||
  die "Universal runtime has no arm64 slice"

print -- "$archs" |
  /usr/bin/grep -qw x86_64 ||
  die "Universal runtime has no x86_64 slice"

extracted_arm="$work_root/extracted-arm64"
extracted_x86="$work_root/extracted-x86_64"

/usr/bin/lipo \
  "$universal" \
  -thin arm64 \
  -output "$extracted_arm"

/usr/bin/lipo \
  "$universal" \
  -thin x86_64 \
  -output "$extracted_x86"

[[ "$(sha256_file "$extracted_arm")" == "$arm_sha" ]] ||
  die "lipo did not preserve arm64 thin binary"

[[ "$(sha256_file "$extracted_x86")" == "$x86_sha" ]] ||
  die "lipo did not preserve x86_64 thin binary"

universal_sha="$(sha256_file "$universal")"

echo "Universal unsigned SHA256 = $universal_sha"

echo
echo "===== ACQUIRE PINNED MODEL ====="

if [[ -n "${MOZKEY_ZENZ_MODEL_SOURCE:-}" ]]; then
  model_source="${MOZKEY_ZENZ_MODEL_SOURCE:A}"

  [[ -f "$model_source" ]] ||
    die "MOZKEY_ZENZ_MODEL_SOURCE does not exist: $model_source"

  /usr/bin/install \
    -m 0644 \
    "$model_source" \
    "$output_model"

  echo "Model source = local verified source"
else
  /usr/bin/curl \
    --fail \
    --location \
    --retry 3 \
    --retry-delay 2 \
    --output "$output_model" \
    "$MODEL_URL"

  echo "Model source = pinned remote revision"
fi

actual_model_sha="$(sha256_file "$output_model")"

echo "Model SHA256 = $actual_model_sha"

[[ "$actual_model_sha" == "$MODEL_SHA256" ]] ||
  die "Pinned model SHA-256 mismatch"

echo
echo "===== NATIVE APPLE SILICON BUILDER SMOKE TEST ====="

# Keep the release output unsigned. Sign a temporary copy ad-hoc only for the
# native execution test after lipo.
/bin/cp "$universal" "$smoke_runtime"
/bin/chmod 0755 "$smoke_runtime"

/usr/bin/codesign \
  --force \
  --sign - \
  "$smoke_runtime"

/usr/bin/codesign \
  --verify \
  --strict \
  --verbose=4 \
  "$smoke_runtime"

python3 - "$smoke_runtime" "$output_model" "$logs/native-arm64-smoke.log" <<'PY'
import http.client
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

llama = Path(sys.argv[1]).resolve()
model = Path(sys.argv[2]).resolve()
log_path = Path(sys.argv[3]).resolve()

api_key = "mozkey-formal-runtime-builder-smoke"

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]

command = [
    "/usr/bin/arch",
    "-arm64",
    str(llama),
    "-m",
    str(model),
    "-c",
    "256",
    "-t",
    "4",
    "--host",
    "127.0.0.1",
    "--port",
    str(port),
    "--api-key",
    api_key,
]

def request(method, path, body=None, timeout=5):
    connection = http.client.HTTPConnection(
        "127.0.0.1",
        port,
        timeout=timeout,
    )

    headers = {
        "Authorization": f"Bearer {api_key}",
    }

    if body is not None:
        headers["Content-Type"] = "application/json"

    try:
        connection.request(
            method,
            path,
            body=body,
            headers=headers,
        )
        response = connection.getresponse()
        data = response.read()
        return response.status, data
    finally:
        connection.close()

with log_path.open("wb") as log:
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )

try:
    deadline = time.monotonic() + 180.0

    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"llama-server exited early: {process.returncode}"
            )

        try:
            status, payload = request(
                "GET",
                "/health",
                timeout=5,
            )

            if status == 200:
                decoded = json.loads(payload.decode("utf-8"))
                if decoded.get("status") == "ok":
                    break
        except Exception:
            pass

        time.sleep(1.0)
    else:
        raise RuntimeError("llama-server health readiness timeout")

    body = json.dumps(
        {
            "prompt": "\uee02\uee00テスト\uee01",
            "n_predict": 8,
            "stream": False,
        },
        ensure_ascii=False,
    ).encode("utf-8")

    status, payload = request(
        "POST",
        "/completion",
        body=body,
        timeout=60,
    )

    if status != 200:
        raise RuntimeError(
            f"completion HTTP status {status}: {payload[:500]!r}"
        )

    decoded = json.loads(payload.decode("utf-8"))

    if "content" not in decoded:
        raise RuntimeError(
            f"completion response has no content field: {decoded!r}"
        )

    print("Native arm64 health     = PASS")
    print("Native arm64 completion = PASS")
    print("Completion chars        =", len(decoded["content"]))

finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
PY

echo
# The exact opt-in switch must be present in both universal slices.
for thin in "$extracted_arm" "$extracted_x86"; do
  /usr/bin/strings "$thin" | /usr/bin/grep -F --     '--mozkey-zenz-dual-liveness-v1' >/dev/null ||
    die "Missing dual-liveness opt-in marker in $thin"
done

echo "===== WRITE BUILD CONTRACT ====="

builder_sha="$(sha256_file "$script_path")"
cmake_version="$(cmake --version | /usr/bin/head -1 | sanitize_contract_value)"
xcode_version="$(/usr/bin/xcodebuild -version | sanitize_contract_value)"
macos_version="$(/usr/bin/sw_vers -productVersion | sanitize_contract_value)"
compiler_version="$(/usr/bin/c++ --version | /usr/bin/head -1 | sanitize_contract_value)"

cat > "$contract" <<CONTRACT
format=$CONTRACT_FORMAT
builder_script_sha256=$builder_sha
llama_cpp_repository=$LLAMA_REPOSITORY
llama_cpp_tag=$LLAMA_TAG
llama_cpp_commit=$LLAMA_COMMIT
zenz_patch_sha256=$PATCH_SHA256
patched_llama_vocab_sha256=$PATCHED_SOURCE_SHA256
mozkey_dual_liveness_patch_sha256=$DUAL_LIVENESS_PATCH_SHA256
patched_llama_server_main_sha256=$DUAL_LIVENESS_MAIN_SHA256
model_repository=$MODEL_REPOSITORY
model_revision=$MODEL_REVISION
model_remote_file=$MODEL_REMOTE_FILE
model_output_file=$MODEL_OUTPUT_FILE
model_sha256=$actual_model_sha
minimum_macos=$MINIMUM_MACOS
architectures=x86_64,arm64
arm64_thin_sha256=$arm_sha
x86_64_thin_sha256=$x86_sha
universal_unsigned_sha256=$universal_sha
signing=downstream_after_lipo
build_host_arch=$host_arch
build_host_macos=$macos_version
cmake_version=$cmake_version
xcode_version=$xcode_version
compiler_version=$compiler_version
CONTRACT

echo
echo "===== FINAL OUTPUT AUDIT ====="

/usr/bin/file "$universal"
echo "Architectures = $(/usr/bin/lipo -archs "$universal")"
echo "arm64 minOS   = $(extract_minos "$extracted_arm")"
echo "x86_64 minOS  = $(extract_minos "$extracted_x86")"
echo "Contract SHA  = $(sha256_file "$contract")"

echo
echo "===== RESULT ====="
echo "Output root                = $output_root"
echo "Builder script SHA256      = $builder_sha"
echo "llama.cpp commit           = $LLAMA_COMMIT"
echo "Patch SHA256               = $PATCH_SHA256"
echo "arm64 thin SHA256          = $arm_sha"
echo "x86_64 thin SHA256         = $x86_sha"
echo "Universal unsigned SHA256  = $universal_sha"
echo "Model SHA256               = $actual_model_sha"
echo "Architectures              = $archs"
echo "Minimum macOS              = $MINIMUM_MACOS"
echo "Dynamic libraries          = SYSTEM ONLY"
echo "Tokenizer marker           = PRESENT BOTH SLICES"
echo "Native arm64 smoke         = PASS"
echo "Signing                    = NOT PERFORMED ON RELEASE OUTPUT"
echo "BUILD-CONTRACT             = $contract"
echo
echo "Formal macOS Zenz Universal runtime built from pinned source successfully."
