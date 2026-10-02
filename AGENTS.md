# Agent notes

- Read `SPECIFICATION.md` and `IMPLEMENTATION_PLAN.md` before changing behaviour. Record every deviation in the plan.
- C++20, CMake, no GStreamer in the media path.
- MXL pin is `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` with fabrics off. One `MXL_REF` in `docker/Dockerfile` and `.github/workflows/ci.yaml`.
- nmos-cpp pin is `fe303849527394b03bdedc8f161f377fe458bb62`.
- Unit tests do not need libmxl. `tests/integration/replay.sh` runs the CPU binary.
- Do not write into a mirror domain (`x-mxl-fabrics-agent.mirror`).
- Config precedence is environment > `REPLAY_CONFIG_FILE` > defaults. Invalid config exits 78. Storage that cannot be created exits 75. SIGTERM exits 143.
