#!/usr/bin/env bash
set -Eeuo pipefail
exec /bin/bash "$(dirname -- "${BASH_SOURCE[0]}")/start-server.sh" frontend
