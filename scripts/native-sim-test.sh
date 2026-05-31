#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

image="${ZEPHYR_DOCKER_IMAGE:-docker.io/zephyrprojectrtos/ci:v0.29.2}"
workspace="${RIGLINK_DOCKER_WORKSPACE:-/workdir}"
repo_in_container="$workspace/modules/lib/riglink"

platform_flag=()
if [[ -n "${RIGLINK_DOCKER_PLATFORM:-}" ]]; then
    platform_flag=(--platform "$RIGLINK_DOCKER_PLATFORM")
elif [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]]; then
    platform_flag=(--platform linux/amd64)
fi

device_flags=()
pytest_args=("$@")
if [[ -n "${RIGLINK_DEVICE:-}" ]]; then
    device_flags=(--device "$RIGLINK_DEVICE")
    pytest_args+=("--riglink-port=$RIGLINK_DEVICE")
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required to run native_sim tests in a Linux container" >&2
    exit 127
fi

printf 'native_sim docker: image=%s' "$image"
if ((${#platform_flag[@]})); then
    printf ' platform=%s' "${platform_flag[1]}"
fi
if ((${#pytest_args[@]})); then
    printf ' pytest_args=%q' "${pytest_args[*]}"
fi
printf '\n'

docker_args=(--rm --init)
if ((${#platform_flag[@]})); then
    docker_args+=("${platform_flag[@]}")
fi
if ((${#device_flags[@]})); then
    docker_args+=("${device_flags[@]}")
fi
docker_args+=(
    --mount "type=volume,src=riglink-west,dst=$workspace"
    --mount "type=bind,src=$repo_root,dst=$repo_in_container"
    --mount "type=volume,src=riglink-pip,dst=/root/.cache/pip"
    --mount "type=volume,src=riglink-ccache,dst=/root/.cache/ccache"
    -e CCACHE_DIR=/root/.cache/ccache
    -e PYTHONPYCACHEPREFIX=/tmp/riglink-pycache
)

script_args=(bash "$workspace" "$repo_in_container")
if ((${#pytest_args[@]})); then
    script_args+=("${pytest_args[@]}")
fi

docker run "${docker_args[@]}" "$image" \
    bash -lc '
set -euo pipefail

workspace="$1"
repo="$2"
shift 2

cd "$workspace"
west_root="$(dirname "$repo")"
mkdir -p "$west_root"

cd "$west_root"
if [[ ! -d .west ]]; then
    west init -l "$repo"
fi

west update zephyr

cd "$repo"
git config --global --add safe.directory "$repo" >/dev/null 2>&1 || true
git submodule update --init --recursive third_party/jcon

python3 -m pip install --quiet -e "./python[dev]"
python3 -m pytest -p no:cacheprovider tests/integration "$@"
' "${script_args[@]}"
