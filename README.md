# Appytizer App Server

Appytizer maps each valid direct subfolder of a configured projects root to
`https://<folder>.test`. It includes a non-elevated FLTK tray UI and a Windows
Service Engine that owns configuration, hosts entries, certificates, nginx,
PHP, and detected database services.

HTTPS is enabled by default. Appytizer creates its own machine-wide development
CA and an exact certificate for each site, while valid HTTP requests redirect
to HTTPS with status `308`. HTTPS can be disabled in Settings; Appytizer then
serves sites over HTTP only and retains certificates for quick re-enablement.
Appytizer never emits HSTS headers.

## Prerequisites

- Windows 10 or 11, Visual Studio 2022 with Desktop C++ tools, and CMake 3.21+
- vcpkg with `VCPKG_ROOT` set
- nginx installed locally; Appytizer discovers common install paths, registry
  entries, and `PATH`

The vcpkg manifest installs sqlite3, nlohmann-json, spdlog, Catch2, and OpenSSL
for `x64-windows-static`. FLTK is fetched by CMake and pinned to release 1.4.5.
No external `openssl` or `mkcert` executable is used.

## Build and test

```powershell
$env:VCPKG_ROOT = 'C:\Users\you\vcpkg'
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The optional trust-store integration test must run from an elevated terminal:

```powershell
$env:APPYTIZER_RUN_ELEVATED_TLS_TESTS = '1'
.\build\Release\appytizer_tests.exe '[.elevated]'
```

It provisions two same-subject test CAs and verifies that removing one exact
recorded certificate does not remove the other.

## Run locally

Provision the Appytizer CA once through UAC, then start the Engine elevated and
the UI normally:

```powershell
Start-Process .\build\Release\AppytizerEngine.exe -ArgumentList '--provision-tls' -Verb RunAs -Wait
Start-Process .\build\Release\AppytizerEngine.exe -ArgumentList '--run-console' -Verb RunAs
Start-Process .\build\Release\Appytizer.exe -ArgumentList '--show'
```

In Settings, choose the projects root. Each direct child name must be one DNS
label: 1–63 ASCII letters, digits, or hyphens, with no leading or trailing
hyphen. Names that differ only by case collide. Invalid folders remain visible
in Sites with an actionable reason, but receive no hosts entry, certificate, or
nginx server block.

Example layout:

```text
C:\appytizer\hello\index.html
C:\appytizer\php\index.php
```

Checks:

```powershell
curl.exe --noproxy '*' https://hello.test/
curl.exe --noproxy '*' https://php.test/
curl.exe --noproxy '*' -I http://hello.test/
```

The HTTP check should return `308` with a corresponding `https://hello.test/`
location. Edge and Chrome trust the certificates through the Windows machine
trust store. The Settings Repair button reruns provisioning through UAC and
refreshes certificates and nginx configuration.

If another nginx instance already owns port 80 or 443, stop it before starting
Appytizer-managed nginx.

## TLS and service commands

These commands are idempotent and require elevation:

```powershell
AppytizerEngine.exe --provision-tls
AppytizerEngine.exe --remove-tls
AppytizerEngine.exe --install-service
AppytizerEngine.exe --uninstall-service
```

`--provision-tls` preserves an existing valid Appytizer CA and site
certificates. `--remove-tls` uses recorded SHA-1 and SHA-256 fingerprints to
remove only the exact Appytizer trust entry, then deletes Appytizer-owned keys.
The uninstaller stops the service before TLS cleanup and service removal.

## Generated state and diagnostics

- CA, ownership metadata, and leaf certificates:
  `%ProgramData%\Appytizer\certificates`
- Engine-owned config, database, logs, and active/staged nginx trees:
  the Engine account's `%LOCALAPPDATA%\Appytizer`
- Hostname resolution: tagged entries in the Windows `hosts` file; TLS
  provisioning does not replace hostname resolution

```powershell
ping hello.test
Resolve-DnsName hello.test -Type A -Server 127.0.0.1 -DnsOnly
Get-NetTCPConnection -LocalPort 80,443 -State Listen
```

Before activation, the Engine generates a complete staging tree and runs
`nginx -t`. A certificate or nginx validation failure leaves the last active
tree untouched. Unknown HTTP hosts return `404`; unknown TLS hosts reject the
handshake.
