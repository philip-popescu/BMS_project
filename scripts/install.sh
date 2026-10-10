#!/usr/bin/env bash
# Run as the desktop user: bash scripts/install.sh
set -Eeuo pipefail
trap 'echo "Installation stopped at line $LINENO. Existing household data was not removed." >&2' ERR

BMS_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SKIP_SYSTEM=0
AUTOSTART=1
for argument in "$@"; do
  case "$argument" in
    --skip-system-packages) SKIP_SYSTEM=1 ;;
    --no-autostart) AUTOSTART=0 ;;
    *) echo "Usage: bash scripts/install.sh [--skip-system-packages] [--no-autostart]" >&2; exit 2 ;;
  esac
done
if [[ "$EUID" -eq 0 ]]; then
  echo "Run this script as the desktop login user. It uses sudo only for OS packages and hostname setup." >&2
  exit 1
fi
if [[ "$SKIP_SYSTEM" -eq 0 ]]; then
  sudo apt-get update
  sudo apt-get install -y python3 python3-venv python3-pip nodejs npm avahi-daemon util-linux
  sudo hostnamectl set-hostname BMS
  sudo systemctl enable --now avahi-daemon
fi

node -e 'const [major,minor]=process.versions.node.split(".").map(Number); if (!((major===20&&minor>=19)||(major===22&&minor>=12)||major>22)) { console.error("Node 20.19+ or 22.12+ is required. Update Node, then re-run this script."); process.exit(1); }'
cd -- "$BMS_ROOT"
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.lock
npm --prefix frontend ci --no-fund --no-audit
npm --prefix frontend run build
mkdir -p data/houses logs

if [[ ! -f .env ]]; then
  # Derive the shared LAN subnet. This route lookup does not contact the internet.
  LAN_INTERFACE="$(ip -4 route show default | awk 'NR==1 {print $5}')"
  if [[ -z "${BMS_LAN_CIDR:-}" ]]; then
    if [[ -z "$LAN_INTERFACE" ]]; then echo "Connect the Pi to the LAN or set BMS_LAN_CIDR before installing." >&2; exit 1; fi
    LAN_ADDRESS="$(ip -4 -o addr show dev "$LAN_INTERFACE" scope global | awk 'NR==1 {print $4}')"
    BMS_LAN_CIDR="$(.venv/bin/python -c 'import ipaddress,sys; print(ipaddress.ip_interface(sys.argv[1]).network)' "$LAN_ADDRESS")"
  fi
  export BMS_LAN_CIDR BMS_ROOT
  umask 077
  .venv/bin/python - <<'PY'
import ipaddress, os, pathlib, shlex
network = ipaddress.ip_network(os.environ['BMS_LAN_CIDR'])
if network.version != 4 or not any(network.subnet_of(ipaddress.ip_network(n)) for n in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16')):
    raise SystemExit('BMS_LAN_CIDR must be a private IPv4 LAN subnet.')
root = pathlib.Path(os.environ['BMS_ROOT'])
settings = {
    'BMS_BIND_HOST': '0.0.0.0',
    'BMS_ALLOWED_NETWORKS': f'127.0.0.0/8,{network}',
    'BMS_ALLOWED_ORIGINS': 'http://BMS:3000,http://bms.local:3000,http://localhost:3000,http://127.0.0.1:3000',
    'BMS_DATA_DIR': str(root / 'data' / 'houses'),
}
timezone = pathlib.Path('/etc/timezone')
if timezone.exists():
    settings['BMS_TIMEZONE'] = timezone.read_text().strip()
(root / '.env').write_text(''.join(f'{key}={shlex.quote(value)}\n' for key,value in settings.items()), encoding='utf-8')
PY
fi

if [[ "$AUTOSTART" -eq 1 ]]; then
  AUTOSTART_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
  mkdir -p "$AUTOSTART_DIR"
  export BMS_ROOT AUTOSTART_DIR
  .venv/bin/python - <<'PY'
import os, pathlib
root = pathlib.Path(os.environ['BMS_ROOT'])
destination = pathlib.Path(os.environ['AUTOSTART_DIR'])
escaped = str(root).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
for name in ('bms-api.desktop', 'bms-frontend.desktop'):
    content = (root / 'deployment' / name).read_text()
    content = content.replace('@BMS_ROOT@', escaped).replace('@BMS_WORKDIR@', str(root))
    (destination / name).write_text(content, encoding='utf-8')
PY
fi

{
  date -u +'%Y-%m-%dT%H:%M:%SZ'
  cat /etc/os-release
  python3 --version
  node --version
  npm --version
  .venv/bin/python -m pip list --format=freeze
} > logs/installation-versions.txt

echo "Installed BMS in $BMS_ROOT"
echo "Existing .env and household JSON data were preserved."
echo "API: http://BMS:2026 | Frontend: http://BMS:3000 (or http://bms.local:3000)"
echo "Start now: bash scripts/start-api.sh and bash scripts/start-frontend.sh in separate terminals."
echo "On next desktop login, both servers start through the installed .desktop files."
echo "If your router does not resolve BMS for Arduino clients, configure their API host with the Pi's LAN IP."
