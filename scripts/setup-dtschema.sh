#!/bin/bash
# Local dtschema install used by "./build.sh check" (dtbs_check). No root needed.
set -euo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
python3 -m venv "$ROOT/.venv-dtschema"
"$ROOT/.venv-dtschema/bin/pip" -q install --upgrade pip dtschema
"$ROOT/.venv-dtschema/bin/dt-doc-validate" --version
echo "dtschema ready: $ROOT/.venv-dtschema"
