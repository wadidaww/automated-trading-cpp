#!/usr/bin/env bash
# OpenD is Futu's own gateway: it is installed and logged in by a human (it needs your Futu
# credentials, SMS/2FA and a device-verification step), so this script only CHECKS that one is
# listening where the trader expects it.
set -euo pipefail
host="${1:-127.0.0.1}"
port="${2:-11111}"
if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
  echo "OpenD is listening on ${host}:${port}"
else
  echo "Nothing is listening on ${host}:${port}." >&2
  echo "Install OpenD from https://openapi.futunn.com, log in, and set its listening address to ${host}:${port}." >&2
  exit 1
fi
