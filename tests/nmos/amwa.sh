#!/usr/bin/env bash
# AMWA IS-04 / IS-05 / BCP-007-03 runner. Not part of default CI.
# Point NMOS_REGISTRY_ADDRESS at a registry and set AMWA_IMAGE to the test harness.
set -euo pipefail
if [[ -z "${AMWA_IMAGE:-}" ]]; then
  echo "Set AMWA_IMAGE to the AMWA NMOS testing container. See IMPLEMENTATION_PLAN.md."
  exit 0
fi
echo "Run IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 against http://${HOST:-127.0.0.1}:${NMOS_PORT:-3302}"
