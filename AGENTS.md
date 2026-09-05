# Hercules

c project.

## Toolchain
GCC 16, CMake 4.4.3, MariaDB client libs, pcre, zlib-ng.

## Commands
```bash
./configure
make -j$(nproc)
```
This is a fork of HerculesWS/Hercules; the working branch is `treeclient-server`, which is why the default branch is not `main`.

## Repo conventions
- Line endings are normalized to LF via `.gitattributes` (these files originated on Windows).
- Managed with the `dev` command: `dev ls`, `dev doctor`, `dev clean Hercules`, `dev rm Hercules`.
