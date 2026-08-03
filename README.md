# Appytizer App Server

Appytizer maps each direct subfolder of a configured projects root to `http://<folder>.local`. It has a FLTK tray UI and a separate Engine that manages local DNS, hosts entries, nginx, PHP, and detected database services.

## Prerequisites

- Windows 10 or 11, Visual Studio 2022 with Desktop C++ tools, and CMake 3.21+
- vcpkg at `C:\Users\<you>\vcpkg` (or another location) with `VCPKG_ROOT` set
- Installed vcpkg packages for `x64-windows-static`: `sqlite3`, `nlohmann-json`, `spdlog`, and Catch2 (the manifest installs these automatically)
- nginx installed locally. Appytizer discovers common install paths, uninstall-registry entries, and `PATH`.

FLTK is fetched by CMake and pinned exactly to `release-1.4.5`.

## Clean build

In PowerShell:

```powershell
$env:VCPKG_ROOT = 'C:\Users\burak\vcpkg' # set once if it is not already defined
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The preset is preferred because it does not depend on shell-specific variable syntax. This also works in PowerShell because `CMakeLists.txt` discovers `VCPKG_ROOT` automatically:

```powershell
cmake -S . -B build -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Do not pass `$env:VCPKG_ROOT/...` literally from Git Bash or cmd: that syntax is PowerShell-only. Use the preset above, or set `VCPKG_ROOT` in the shell first.

## Run locally

The Engine must run elevated in development because Windows requires elevation to update the managed `hosts` entries and NRPT rule. Open PowerShell in the repository and run:

```powershell
Start-Process .\build\Release\AppytizerEngine.exe -ArgumentList '--run-console' -Verb RunAs
Start-Process .\build\Release\Appytizer.exe -ArgumentList '--show'
```

Accept the UAC prompt for the Engine. The UI must not be elevated.

In **Settings**, choose the projects root, for example `C:\appitizer`, then click **Save and apply**. Put each site in a direct child directory:

```text
C:\appitizer\hello\index.html
C:\appitizer\testy\index.html
```

Appytizer watches the configured projects root while the Engine is running. Adding, removing, or renaming a direct project folder automatically updates SQLite, managed hosts entries, nginx virtual-host files, and the visible Sites list; Appytizer-managed nginx is refreshed when it is already running. **Rescan folders** remains available as an explicit recovery action. On Engine startup, the first detected PHP and nginx versions are started automatically; the Dashboard or Services page can still restart them manually.

```powershell
curl.exe --noproxy '*' http://hello.local/
curl.exe --noproxy '*' http://testy.local/
```

If nginx was already started outside Appytizer, it may already own port 80 and serve its stock welcome page. Stop that existing nginx master before starting nginx from Appytizer; only one nginx listener can own port 80.

## Diagnostics

```powershell
# Browser-style resolution uses the managed hosts entry.
ping hello.local

# Query Appytizer's loopback DNS responder directly.
Resolve-DnsName hello.local -Type A -Server 127.0.0.1 -DnsOnly

# Check the active listener on port 80.
Get-NetTCPConnection -LocalPort 80 -State Listen
```

Appytizer-generated nginx files live under `%LOCALAPPDATA%\Appytizer\nginx`. Engine logs are under `%LOCALAPPDATA%\Appytizer\logs`.
