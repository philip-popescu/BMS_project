#!/usr/bin/env bash
set -Eeuo pipefail
BMS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
  api) MODULE=backend.server ;;
  frontend) MODULE=backend.frontend_server ;;
  *) echo "Usage: start-server.sh api|frontend" >&2; exit 2 ;;
esac
cd -- "$BMS_ROOT"
if [[ -f .env ]]; then set -a; source .env; set +a; fi
mkdir -p logs
# A second login or manual launch cannot create a competing server instance.
exec flock -n "logs/$1.lock" "$BMS_ROOT/.venv/bin/python" -m "$MODULE" >> "logs/$1.log" 2>&1
