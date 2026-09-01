# Appytizer App Server

Appytizer is a Windows-native local development environment for projects that
need friendly local domains. Point it at a projects folder and each direct
child becomes a site such as `https://my-project.test`.

It is conceptually similar to Laravel Valet and Laravel Herd, but is built for
Windows with a native FLTK tray application, a background Windows service,
nginx, and bundled PHP. It is designed to stay out of the way while keeping
local sites, HTTPS, and development services predictable.

## Features

- Automatic local domains for project folders (`<folder>.test`)
- HTTPS by default with an Appytizer-managed local certificate authority
- HTTP-to-HTTPS redirects and optional HTTP-only mode
- Bundled nginx and PHP runtime, with detection of other installed versions
- PHP, MySQL, and PostgreSQL service status in the tray UI
- Native Windows service for hosting and configuration
- Site validation for DNS-safe folder names and required index files
- Automatic nginx configuration and certificate updates when projects change
- Activity and diagnostic information without requiring a browser dashboard

## Low resource footprint

Appytizer is intended to sit quietly in the tray rather than behave like a
full development dashboard. The UI and Engine are native Windows processes,
so they do not require an embedded browser or a JavaScript runtime. As a
reference point, an idle local build measured about **34 MB** for the UI and
**11 MB** for the Engine (roughly **45 MB combined working set**); actual usage
varies with the number of sites, services, and Windows memory conditions.

## Requirements

For users, Appytizer supports 64-bit Windows 10 and Windows 11.

For development, install:

- Visual Studio 2022 with the Desktop C++ workload
- CMake 3.21 or newer
- vcpkg with `VCPKG_ROOT` pointing to its checkout
- Inno Setup 6 (only required for packaging an installer)

Dependencies are declared in `vcpkg.json`. The CMake configuration uses the
`x64-windows-static` triplet and fetches FLTK 1.4.5.

## Build and package locally

From the repository root, run:

```powershell
$env:VCPKG_ROOT = 'D:\tools\vcpkg'
.\scripts\build-installer.ps1
```

The script configures and builds the Release targets, runs the test suite,
downloads the pinned nginx and PHP runtimes when they are not already staged,
copies the repository's `installer/php.ini` into the PHP runtime, checks for
Inno Setup 6, and creates:

```text
installer\Output\AppytizerSetup.exe
```

To skip tests while iterating on packaging:

```powershell
.\scripts\build-installer.ps1 -SkipTests
```

The script searches the normal per-user and system Inno Setup locations and
does not contain machine-specific usernames or paths.

## Run from a build

Provision the local HTTPS certificate authority once from an elevated shell,
then start the Engine elevated and the UI normally:

```powershell
.\build\Release\AppytizerEngine.exe --provision-tls
.\build\Release\AppytizerEngine.exe --run-console
.\build\Release\Appytizer.exe --show
```

Choose a projects root in Settings. Each direct child must be a single DNS
label (letters, digits, and hyphens; no leading or trailing hyphen). A site
serves `index.html` or `index.php` when present.

Example:

```text
C:\Projects\hello\index.html
C:\Projects\catalog\index.php
```

If another program is using ports 80 or 443, stop it before starting
Appytizer-managed nginx.

## Configuration and data

Appytizer keeps runtime state outside the repository:

- `%ProgramData%\Appytizer\certificates` — local CA and site certificates
- `%ProgramData%\Appytizer\php\php.ini` — shared PHP configuration
- `%LOCALAPPDATA%\Appytizer` — Engine configuration, logs, and nginx trees

The installer preserves an existing shared `php.ini` so upgrades do not erase
user changes. New installations receive the tracked bundled configuration,
which enables common PDO and web extensions and uses a 50 MB upload limit.

## GitHub releases

The Windows release workflow runs only for tags pushed to commits on
`master`. It builds and tests the Release configuration, stages nginx and PHP,
compiles the Inno Setup installer, and attaches it to a GitHub Release.

```powershell
git tag v1.0.0
git push origin v1.0.0
```

Tags that are not reachable from `master` are skipped.

## Development notes

The repository is split into the Engine (`engine/`), native UI
(`appytizer-ui/`), shared code (`common/`), tests (`tests/`), and installer
assets. Run the tests with:

```powershell
ctest --preset windows-release --output-on-failure --timeout 60
```

Contributions should keep platform-specific behavior isolated, preserve the
service/UI separation, and include tests for behavior changes.

## License

License terms have not yet been published.
