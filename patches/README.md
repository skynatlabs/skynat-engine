# patches

Patches applied to OBS Studio before building go here, one `.patch` file each, and every one is
listed in `NOTICE.md`. There are none: the engine is built from unmodified OBS Studio 31.0.4.
`scripts/build-macos.sh` and `scripts/build-windows.ps1` refuse to build if a patch appears here
without being applied on purpose.
