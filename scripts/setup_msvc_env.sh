#!/bin/bash
# Source MSVC BuildTools environment then forward to whatever command was
# requested. Usage: source scripts/setup_msvc_env.sh <command> [args...]
set -e
VCVARS="E:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
if [ ! -f "$VCVARS" ]; then
    echo "vcvars64.bat not found at $VCVARS" >&2
    exit 1
fi
# Drive vcvars64.bat under cmd, then dump the resulting environment as
# KEY=VALUE lines so we can re-export it inside bash.
ENV_DUMP="$(mktemp)"
cmd //c "\"$VCVARS\" >nul && set" > "$ENV_DUMP"
while IFS='=' read -r key value; do
    case "$key" in
        PATH|INCLUDE|LIB|LIBPATH|VCINSTALLDIR|VSINSTALLDIR|WindowsSdkDir|UCRTVersion|VSCMD_*)
            export "$key"="$value"
            ;;
    esac
done < "$ENV_DUMP"
rm -f "$ENV_DUMP"
exec "$@"
