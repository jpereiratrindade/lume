#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
unit_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
model=${LUME_SETUP_MODEL:-qwen2.5:3b}

if ! command -v ollama >/dev/null 2>&1; then
    echo "Ollama não foi encontrado no PATH." >&2
    exit 1
fi

install -D -m 0644 \
    "$project_dir/contrib/ollama/lume-ollama.service" \
    "$unit_dir/lume-ollama.service"
systemctl --user daemon-reload
systemctl --user enable --now lume-ollama.service

echo "Preparando o modelo local $model..."
OLLAMA_HOST=http://127.0.0.1:11435 ollama pull "$model"
echo "Lume LLM pronto em http://127.0.0.1:11435/v1"

