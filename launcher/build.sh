#!/bin/sh
HERE="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$HERE" PAYLOAD=a6_nx exec "$HERE/../runtime/launcher/build.sh" "$@"
