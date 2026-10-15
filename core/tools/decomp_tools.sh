#!/usr/bin/env bash
# The GameCube / Wii decompilation toolchain into /root/decomp-tools (WSL): decomp-toolkit (dtk), objdiff-cli, m2c.
#   bash core/tools/decomp_tools.sh   (re-run to update; checked 2026-10-15: dtk 1.8.4, objdiff-cli 3.8.2, m2c 708d2d2)
# dtk reads discs directly: dtk vfs cp "IMAGE:sys/main.dol" main.dol; m2c: venv/bin/python m2c/m2c.py -t ppc-mwcc-c f.s
# Release assets come from the projects' own GitHub releases (encounter/decomp-toolkit, encounter/objdiff); m2c is a
# Python project cloned from its repository (matt-kempster/m2c).
set -euo pipefail
d=/root/decomp-tools
mkdir -p "$d"
cd "$d"
asset() {  # repo, name pattern -> the latest release's download URL
    curl -sL "https://api.github.com/repos/$1/releases/latest" | python3 -c "
import json, sys, re
r = json.load(sys.stdin)
print(r.get('tag_name', ''), file=sys.stderr)
for a in r.get('assets', []):
    if re.search(sys.argv[1], a['name']):
        print(a['browser_download_url']); break" "$2"
}
u="$(asset encounter/decomp-toolkit 'linux-x86_64$')"
echo "dtk: $u"; curl -sL -o dtk "$u"; chmod +x dtk; ./dtk --version
u="$(asset encounter/objdiff 'objdiff-cli-linux-x86_64$')"
echo "objdiff-cli: $u"; curl -sL -o objdiff-cli "$u"; chmod +x objdiff-cli; ./objdiff-cli --version || true
if [ ! -d m2c ]; then git clone -q --depth 1 https://github.com/matt-kempster/m2c.git; fi
(cd m2c && git log -1 --format='m2c %h %cd' --date=short)
python3 -m venv venv
./venv/bin/pip -q install -r m2c/requirements.txt 2>/dev/null || ./venv/bin/pip -q install pycparser graphviz
./venv/bin/python m2c/m2c.py --help | head -3
ls -la "$d"
