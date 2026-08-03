# Appytizer App Server — PRD & Implementation Plan


## 0. How the coding agent should use this document

This file is the single source of truth for scope and sequencing. As each task below is completed:

1. Change its checkbox from `- [ ]` to `- [x]`.
2. Add a one-line note under the task if a decision deviated from the plan (e.g. `> Note: used X instead of Y because...`).
3. Do not skip ahead to a later phase while earlier phase checkboxes remain unchecked, unless a task is explicitly marked `(parallelizable)`.
4. Keep this file in the repo root (e.g. `PROGRESS.md` or `PRD.md`) and commit it alongside the code changes it describes.

All C++ code produced against this plan should follow §10 (C++ Best Practices) without being asked again per-task.

---

## 1. Product Summary

**What it is:** A Windows tray utility that serves any subfolder of a configured root directory at `http://foldername.local` (or a user-chosen extension), auto-detecting and managing local dev services (nginx, PHP, MySQL, PostgreSQL, MongoDB) with a modular architecture so new languages/services (Python, Node.js, etc.) can be added later. (Similar to Valet, Laragon)

**Explicitly descoped for v1** (confirmed by product owner):
- Per-site version pinning (e.g. site A on PHP 8.3, site B on PHP 8.4). Version selection is **global per service** — one active PHP version, one active MySQL version, etc., at a time.
- Bundling PHP/nginx/phpMyAdmin inside the installer (roadmap item, not v1).
- Installer creation is now **in scope** (previously roadmap), see §9 (only after everything is implemented and tested).

**Primary user:** the developer themself, running many small projects locally, switching between them by folder name.

---

## 2. Architecture Overview

Two separate binaries, communicating over a local named pipe:

```
┌─────────────────────────┐        Named Pipe (JSON)        ┌──────────────────────────────┐
│   Appytizer UI (unprivileged) │ ───────────────────────────────▶│  Engine (Windows Service,     │
│   FLTK, runs per-user,   │◀─────────────────────────────── │  runs as LocalSystem/elevated) │
│   no admin rights needed │        status / events          │                                │
└─────────────────────────┘                                  │  - DNS server (UDP/53, loopback)│
                                                               │  - hosts file mirror            │
                                                               │  - NRPT registry policy         │
                                                               │  - nginx process supervision    │
                                                               │  - ServiceProvider plugins       │
                                                               │    (PHP, MySQL, Postgres, Mongo)│
                                                               │  - Job Objects + RAM polling     │
                                                               │  - SQLite site registry          │
                                                               └──────────────────────────────┘
```

**Why this split (do not collapse back into one process):**
- DNS on port 53, hosts file writes, and NRPT registry policy all require elevation. The UI should never run elevated — that's a UAC prompt on every login and a bad look for an open-source tray app.
- The Engine must survive the UI closing (`run minimized` / user closes window) and must survive login/logoff, which only a Windows Service gives you cleanly.
- If the UI (FLTK, third-party libs, user-triggered redraw bugs) crashes, running services and DNS must **not** go down with it.

- [x] Confirm this two-process model before writing engine code (architecture sign-off checkpoint)
> Note: retained the service/UI split; privileged integration stays in `AppytizerEngine`.

---

## 3. Tech Stack

| Concern | Choice | Notes |
|---|---|---|
| Language | C++20 | `<filesystem>`, structured bindings, `std::optional`/`std::expected`-style error handling |
| UI | FLTK (existing template) | Retained, restyled per §7 |
| Local DB | SQLite | Site registry + optional RAM history only — **not** general settings storage |
| Settings | Plain JSON file (`config.json`) via **nlohmann/json** | Human-editable, no migrations needed for simple key/value |
| IPC serialization | nlohmann/json over named pipe | Simple envelope, see §6 |
| Logging | **spdlog** (engine primarily; UI optional) | Rotating file sink under `%LOCALAPPDATA%\Appytizer\logs` |
| Build | CMake + vcpkg | vcpkg for sqlite3, nlohmann-json, spdlog; FLTK can stay as your existing setup |
| Testing | Catch2 or GoogleTest | Unit-test the Engine's non-UI logic: detection parsers, DNS packet handling, config serialization |

- [x] Set up CMake project with vcpkg manifest (`vcpkg.json`) pinning the above dependencies
> Note: installed vcpkg packages are used for SQLite, JSON, and spdlog; FLTK is source-pinned to `release-1.4.5`.

---

## 4. Repository Layout

```
/engine/                 # Windows Service executable
    /dns/                 # DNS packet parsing + forwarding
    /services/            # ServiceProvider implementations (one file per service)
    /nginx/               # config generation, reload orchestration
    /ipc/                 # named pipe server, message handling
    /db/                  # SQLite access layer (site registry)
    /jobobjects/          # process supervision, RAM polling
    main.cpp
/appytizer-ui/            # FLTK application (existing template lives here, restyled)
    /views/               # Dashboard, Services, Sites, Settings
    /widgets/             # ServiceCard, SiteRow, StatusPill, RoundedButton
    /ipc/                 # named pipe client
    main.cpp
/common/                 # Shared between engine & UI: IPC message structs, config schema
/assets/                 # PNG icons (existing), .ico for tray
/installer/
    installer.iss
/docs/
    PRD_Implementation_Plan.md   # this file
```

- [x] Create the above directory skeleton
- [x] Move existing `main.cpp` content into `/appytizer-ui/`, split GUI-only code from anything that touches services/DNS/hosts (there shouldn't be much yet — this is prep for §7)

---

## 5. Engine Design

### 5.1 Windows Service host

- [x] Implement `ServiceMain`/`ServiceCtrlHandler` boilerplate (standard Win32 service pattern)
- [x] Add CLI switches on the engine binary itself:
  - `--install-service` (calls `CreateServiceW`, sets `SERVICE_AUTO_START`, configures `ChangeServiceConfig2` with `SERVICE_CONFIG_FAILURE_ACTIONS` → restart on crash after e.g. 5s, up to N times)
  - `--uninstall-service` (stop + `DeleteService`)
  - `--run-console` (run in foreground for local debugging, no SCM dependency)
- [x] Service name/display name constants centralized in `/common`

### 5.2 `ServiceProvider` plugin interface

This is the extensibility seam for PHP/MySQL/Postgres/MongoDB now, Python/Node later.

```cpp
// common/service_provider.hpp
struct InstalledVersion {
    std::string version_label;   // e.g. "8.3.14"
    std::filesystem::path executable_path;
    bool is_windows_service;     // true = MySQL/Postgres installed via official installer
    std::string windows_service_name; // valid only if is_windows_service
};

struct ServiceStatus {
    bool running{false};
    std::string active_version;
    std::vector<DWORD> process_ids; // all PIDs in this service's Job Object (or SCM-reported PID)
    std::size_t working_set_bytes{0};
};

class IServiceProvider {
public:
    virtual ~IServiceProvider() = default;
    virtual std::string id() const = 0;                 // "php", "mysql", "nginx", ...
    virtual std::string display_name() const = 0;        // "PHP", "MySQL", ...
    virtual std::vector<InstalledVersion> detect() = 0;   // scan registry/paths/PATH
    virtual bool start(const std::string& version_label) = 0;
    virtual bool stop() = 0;
    virtual bool restart(const std::string& version_label) = 0;
    virtual ServiceStatus status() const = 0;
};
```

- [x] Define `IServiceProvider` in `/common/service_provider.hpp`
- [x] Implement a `ServiceRegistry` that holds `vector<unique_ptr<IServiceProvider>>`, exposes `getById`, `all()`, and drives detection on engine startup
- [x] Implement `NginxProvider` (portable binary, `CreateProcess`-managed — special-cased since nginx isn't user-selectable in the "installed services" sense, but fits the same interface for status/start/stop/restart)
- [x] Implement `PhpProvider` (portable `php-cgi.exe`, detects multiple versions under a configured PHP root, e.g. `C:\tools\php\83`, `C:\tools\php\84`; only one active at a time per v1 scope)
- [ ] Implement `MySqlProvider` (detect via Windows Service enumeration + registry `Uninstall` keys + common install paths; start/stop via SCM `ControlService` since these are pre-existing Windows Services)
- [x] Implement `PostgresProvider` (same pattern as MySQL)
- [x] Implement `MongoProvider` (same pattern as MySQL)
- [x] Detection runs once on Engine startup and is re-triggerable via an IPC "rescan" command (for "I just installed MySQL, refresh")
> Note: database providers currently use SCM enumeration; registry-uninstall and PATH probes remain follow-up work.

**Detection strategy per provider** (layer these, first match wins or merge results):
1. Windows Service enumeration (`EnumServicesStatusExW`) — filter known service name patterns (`MySQL*`, `postgresql-x64-*`, `MongoDB`)
2. Registry `Uninstall` keys (`HKLM\...\Uninstall\*`) for display name + install location
3. Common install directories (`C:\Program Files\MySQL`, `C:\php`, etc. — make these configurable, not hardcoded, per §5.8)
4. `PATH` scan + spawn `<exe> --version`, parse stdout

- [ ] Implement the layered detection helper shared across providers (`/engine/services/detection_utils.*`)

### 5.3 Process supervision & Job Objects

- [ ] Wrap every engine-spawned process (nginx, php-cgi) in its own Win32 **Job Object** (`CreateJobObjectW`, `AssignProcessToJobObject`) so:
  - closing the Engine reliably kills all children (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`)
  - `QueryInformationJobObject` with `JobObjectBasicProcessIdList` gives you every PID (including child processes spawned by nginx workers) for RAM aggregation
- [ ] Capture stdout/stderr via anonymous pipes for basic log capture (feeds §7.3 log view, nice-to-have)

### 5.4 RAM usage tracking

- [ ] Poll `GetProcessMemoryInfo` (psapi.dll) for every PID in each service's Job Object on a timer (e.g. every 2s), sum `WorkingSetSize`
- [ ] For SCM-managed services (MySQL/Postgres/Mongo), resolve PID via `QueryServiceStatusEx` (`dwProcessId`) instead of a Job Object
- [ ] Push aggregated numbers to UI via IPC status broadcast (§6)

### 5.5 Local DNS Server

- [x] Implement a minimal UDP DNS responder bound to **`127.0.0.1:53` only** (never `0.0.0.0`)
- [x] Parse incoming queries (simple DNS wire format — header + question section is enough for this use case)
- [x] If query name ends with configured extension (default `.local`) → respond with an A record `127.0.0.1`
- [x] If query type is AAAA for the configured extension → respond with an empty answer (not silence) to avoid client-side IPv6 timeout stalls
- [ ] If query does not match the extension → this should rarely happen once NRPT scoping (below) is in place, but as a safety net, forward the raw query to the original upstream DNS server (captured once at first run, before any policy is applied) and relay the response back
- [ ] On start: read current adapter DNS servers (for the forwarding fallback) **before** applying NRPT policy
- [ ] Call `DnsFlushResolverCache()` (dnsapi.dll) after any hosts-file or NRPT change to avoid stale cached results during testing

### 5.6 NRPT scoping (replaces "override adapter DNS" approach)

- [x] On Engine start (or when extension setting changes), write an NRPT rule scoping only the configured extension to `127.0.0.1`, via direct registry write to `HKLM\SOFTWARE\Policies\Microsoft\Windows NT\DNSClient\DnsPolicyConfig\<GUID>` (mirrors what `Add-DnsClientNrptRule` does under the hood — do this natively, don't shell out to PowerShell)
- [x] On extension change or uninstall, remove the old rule before adding/leaving
- [ ] On Engine `--uninstall-service` / uninstaller run, remove the NRPT rule and any hosts file entries this app added (tag them with a recognizable comment, e.g. `# Appytizer:foldername`, so removal only touches app-managed lines)

### 5.7 Hosts file mirroring (safety net)

- [x] On each detected site (folder), also append/refresh a `hosts` file entry (`127.0.0.1  foldername.local  # Appytizer`) — this means already-registered sites keep resolving even during a brief Engine DNS hiccup, since Windows checks `hosts` before querying DNS
- [x] Deduplicate/clean stale entries when a folder is removed or renamed (compare against SQLite site registry)

### 5.8 nginx config generation

- [ ] Root nginx config `includes` one generated file per detected site (not one wildcard block), each with `root` pointing at the folder and `fastcgi_pass` pointing at the currently active global PHP version's port
- [ ] On any site add/remove or PHP version switch, regenerate the affected config file(s) and run `nginx -s reload` (graceful — no dropped connections), not a full restart
- [ ] Static asset serving (CSS/JS/images) falls out of the standard `root` + `try_files` directives already in the template config — no special-casing needed beyond optional cache headers

### 5.9 Config & data storage

- `config.json` (plain file, via nlohmann/json) — app settings: root folder path, chosen extension, run-minimized flag, autostart flag, active version per service
- SQLite — site registry table (`sites: id, folder_name, path, detected_type, first_seen, last_seen`) and optional `ram_history` table for future sparkline nice-to-have

- [x] Define `config.json` schema and a small `ConfigStore` load/save class
- [x] Define SQLite schema + migration-free `CREATE TABLE IF NOT EXISTS` init (matches existing template's `Database` class pattern)

---

## 6. IPC Protocol (Engine ⟷ Appytizer UI)

Named pipe, e.g. `\\.\pipe\AppytizerEngine`. Newline-delimited JSON messages, request/response + a push channel for status updates.

```json
// Request
{ "id": "1", "cmd": "service.start", "params": { "service": "php", "version": "8.3.14" } }

// Response
{ "id": "1", "ok": true, "result": {} }

// Unsolicited status push (broadcast on change or every N seconds)
{ "event": "status.update", "services": [
    { "id": "nginx", "running": true, "ram_mb": 42, "version": "1.25.3" },
    { "id": "php", "running": true, "ram_mb": 88, "version": "8.3.14", "available_versions": ["8.3.14","8.4.1"] }
  ],
  "dns": { "running": true, "port": 53, "extension": ".local" }
}
```

Command set to implement: `service.list`, `service.start`, `service.stop`, `service.restart`, `service.set_version`, `dns.start`, `dns.stop`, `dns.restart`, `sites.list`, `sites.rescan`, `config.get`, `config.set`, `stop_all`.

- [x] Implement named pipe **server** in Engine (async, `ReadFile`/`WriteFile` overlapped or a simple worker-thread-per-connection model since connection count is trivial)
- [x] Implement named pipe **client** in Appytizer UI
- [x] **Critical FLTK threading note:** FLTK is not thread-safe. IPC responses/pushes arriving on a background thread must be marshaled to the UI thread via `Fl::awake(callback, data)` — never touch FLTK widgets directly from the pipe-reading thread.

- [x] Add this threading rule as a comment at the top of the UI's IPC client file so it isn't relitigated later

---

## 7. Appytizer UI Design

### 7.1 Visual Design System

**Replace the existing color constants in `main.cpp` with these.** Rationale: deeper near-black background (vs. the current navy-gray) reads as more "premium dev tool" (Linear/Raycast/VSCode-dark territory), a single accent color used consistently for all interactive/active states, and a dedicated danger palette instead of reusing a raw RGB inline.

| Constant | Old value | New value (hex) | Usage |
|---|---|---|---|
| `kBackground` | `rgb(16,23,34)` | `#0B0E14` | App canvas |
| `kPanel` | `rgb(24,35,48)` | `#141821` | Card / panel fill |
| `kPanelHover` | `rgb(34,49,65)` | `#1C212B` | Card hover / pressed state |
| `kBorder` | `rgb(49,64,83)` | `#242A35` | Card & input borders (1px) |
| `kText` | `rgb(232,240,248)` | `#E7ECF3` | Primary text |
| `kMuted` | `rgb(129,149,170)` | `#8792A2` | Secondary/meta text |
| `kGreen` (rename → `kAccent`) | `rgb(40,174,154)` | `#22D3B0` | Primary accent — buttons, active nav item, running-status dot |
| *(new)* `kAccentHover` | — | `#3EE6C4` | Accent hover state |
| *(new)* `kSuccess` | — | `#34D399` | "Running" status pill |
| *(new)* `kWarning` | — | `#FBBF24` | "Starting/Restarting" status pill |
| *(new)* `kDanger` | — | `#F87171` | "Stopped/Error" status pill, destructive button text |
| *(new)* `kDangerBg` | — | `#3A1620` | Destructive button fill (e.g. "Reset data", "Stop") |
| *(new)* `kRadiusCard` | — | `12` px | Corner radius for cards |
| *(new)* `kRadiusButton` | — | `8` px | Corner radius for buttons/pills |
| *(new)* `kRadiusChip` | — | `999` px (fully round) | Status pills, version-select chips |

- [x] Apply the table above to `main.cpp`, renaming `kGreen` → `kAccent` throughout
- [x] Bump card/button corner radii in existing `fl_rounded_rectf`/`fl_rounded_rect` calls to use `kRadiusCard`/`kRadiusButton` instead of the hardcoded `5`

**Typography:** load "Segoe UI" (Windows-native, modern, free) instead of relying on FLTK's default Helvetica mapping:

```cpp
Fl::set_font(FL_FREE_FONT, "Segoe UI");
// use FL_FREE_FONT (and FL_FREE_FONT + FL_BOLD via a second slot) in place of FL_HELVETICA
```

- [ ] Load Segoe UI as above; keep FL_HELVETICA as a silent fallback if the font isn't found (rare, but handle gracefully — don't crash)

**Spacing:** standardize on an 8px grid (8/16/24/32) for margins/padding instead of the current ad-hoc pixel values, for visual consistency across the new tabs.

- [ ] Introduce spacing constants (`kSpaceSm=8, kSpaceMd=16, kSpaceLg=24, kSpaceXl=32`) and use them when laying out the new views

### 7.2 Layout — sidebar navigation

Replace the current single-window `Fl_Group` show/hide dashboard/settings pattern with a **persistent left sidebar + swappable content area**, since we're going from 2 views to 5:

```
┌──────┬──────────────────────────────────────────┐
│      │  DASHBOARD                                │
│ [🏠]  │  ┌─────────────┐ ┌─────────────┐          │
│ [⚙]  │  │ Local DNS    │ │ nginx        │  ...    │
│ [📁]  │  │ ● Running    │ │ ● Running    │         │
│ [🗄]  │  │ Start/Stop   │ │ Start/Stop   │         │
│      │  └─────────────┘ └─────────────┘          │
│      │                                            │
└──────┴──────────────────────────────────────────┘
  72px          remaining width, min window ~960×600
```

- Sidebar: icon-only, ~72px wide, uses the existing PNG assets. Active item gets a rounded highlight rect behind the icon in `kAccent` at ~15% opacity, icon itself tinted `kAccent` when active, `kMuted` when inactive.
- Suggested nav items: **Dashboard**, **Services**, **Sites**, **Settings** (DNS status can live as a card on Dashboard rather than needing its own tab — simpler).
- Content area: swap `Fl_Group`s exactly like the existing template's `dashboard_`/`settings_` show/hide pattern, just with more groups.

- [x] Increase main window size (suggest 960×600, resizable)
- [x] Build sidebar widget with the 4 nav icons, wire click → show/hide the corresponding content group (extends existing `show_settings_cb`/`show_dashboard_cb` pattern)
> Note: compact glyphs are used until a complete tintable four-icon PNG set is available.
- [ ] Load PNGs via `Fl_PNG_Image` for sidebar icons (existing `assets/*.png`); keep the separate `.ico` resource for the actual `Shell_NotifyIconW` tray icon — **PNG and the Win32 tray HICON are not interchangeable**, don't try to feed a PNG directly into `NOTIFYICONDATAW`

### 7.3 Views

**Dashboard** — at-a-glance grid of `ServiceCard`s (one per detected service + one for "Local DNS Server" itself), each showing: service icon, name, status pill, active version, RAM usage (live, updated from IPC push), Start/Stop/Restart icon buttons.

- [x] Build `ServiceCard` widget (reusable for every service including DNS)
- [x] Wire IPC `status.update` push → update card contents without full UI rebuild (update labels/redraw only the changed widgets)

**Services** — same cards as Dashboard but with the version-selector chip exposed (dropdown of `available_versions` from detection) for services that have multiple installed versions (e.g. PHP 8.3 / 8.4). Selecting a version calls `service.set_version` then `service.restart`.

- [ ] Build version-selector chip/dropdown widget
- [ ] Wire selection → IPC `service.set_version`

**Sites** — table/list of detected folders under the root, one `SiteRow` per folder:

- [ ] Build `SiteRow` widget: folder name, detected type badge (PHP/Static — inferred by presence of `index.php` vs `index.html`), the resolved URL (`foldername.local`), a **"Copy URL" button** (rounded, small, clipboard glyph) that calls `OpenClipboard`/`SetClipboardData` with `CF_TEXT`, and an "Open in browser" button (`ShellExecuteW` with the URL)
- [x] Wire to IPC `sites.list` on tab open, `sites.rescan` on a manual refresh button
- [x] Live-refresh this list via `ReadDirectoryChangesW` on the root folder in the Engine, pushed as a `sites.changed` event

**Settings**
- [ ] "Run when Windows starts" — existing template logic already does this for the Appytizer UI; note in-code that this only affects the **UI**, since the Engine now autostarts via its own Windows Service registration regardless
- [x] "Run minimized" — new checkbox, stored in `config.json`; if set, `App::show()` is skipped on launch and the app goes straight to tray
- [x] "Extension chooser" — dropdown or text field (`.local`, `.test`, `.dev` etc.), on change triggers `config.set` → Engine re-applies NRPT rule (§5.6) and regenerates hosts entries/nginx configs with the new suffix
- [x] Root folder picker — `IFileOpenDialog` (COM) for folder selection, writes to `config.json`, triggers `sites.rescan`

### 7.4 Tray icon behavior

- [x] Left click → `show_app()` (existing behavior, keep as-is)
- [x] Right click context menu: **Display UI**, **Stop all and exit**, **Exit**
  - "Stop all and exit": IPC `stop_all` (stops every running service + DNS in the Engine) then quits the Appytizer UI. Does **not** stop the Windows Service itself — that's a Settings-level or uninstaller-level action, not a tray action, to avoid surprising the user by disabling their dev environment's autostart.
  - "Exit": just closes the Appytizer UI, Engine keeps running services as-is (this matches "run minimized" intent — closing the window shouldn't kill your DNS/nginx/PHP)

---

## 8. C++ Best Practices (apply throughout, not just once)

- [ ] RAII everywhere for Win32 handles — wrap `HANDLE`/`HKEY`/`SC_HANDLE`/`HICON` in small RAII guard types (unique_ptr with custom deleter, or a tiny `WinHandle` wrapper) rather than manual `CloseHandle`/`RegCloseKey` calls scattered through logic
- [ ] No raw `new`/`delete` — smart pointers (`unique_ptr` for ownership, raw pointers/references for non-owning views) throughout new code
- [ ] Const-correctness on all methods that don't mutate state (existing template already does this well — keep it up)
- [ ] Consistent error handling: prefer returning `bool`/`std::optional<T>`/small result structs over exceptions for expected failure paths (service not installed, pipe not connected); reserve exceptions for truly exceptional cases. Log failures via spdlog rather than silently swallowing them (existing template silently discards sqlite errors in a few places — fix in the new Engine code, and consider back-filling)
- [ ] Doxygen-style `/// comments` on every public class/method in `/common` and `/engine` headers, since this is an open-source, multi-contributor codebase
- [ ] Anonymous namespaces or a proper `namespace Appytizer { ... }` for translation-unit-local helpers (existing template's `namespace { ... }` pattern is fine, keep it)
- [ ] Avoid busy-wait polling loops — use `WaitForSingleObject`/`WaitForMultipleObjects` with timeouts, or FLTK's `Fl::add_timeout`, rather than sleep-loops
- [ ] Thread safety: any state shared between the pipe-reading thread and the FLTK main thread must be protected (mutex or lock-free queue) and marshaled via `Fl::awake` — see §6 note
- [ ] Header/source separation for everything in `/engine` and `/common` (the Appytizer UI can keep the template's more compact single-file style per view if that matches your existing conventions)

---

## 9. Installer (Inno Setup)

Confirmed: Inno Setup is sufficient. Two non-obvious wrinkles to handle explicitly:

- [x] `PrivilegesRequired=admin` at the top of `installer.iss` (service registration + registry policy writes require elevation)
- [x] `[Code]` section: before file extraction, check if the Engine service exists and is running; if so, stop it (`ServiceStop`-style Pascal helper or `Exec('sc.exe', 'stop AppytizerEngine', ...)`) so the binary isn't locked during copy
- [x] `[Run]` section: after files are copied, call `enginename.exe --install-service` (idempotent — safe to call again on upgrade) then start it
- [ ] `[UninstallRun]` section: call `enginename.exe --uninstall-service` **before** `[UninstallDelete]` removes files, so the service is cleanly stopped/deregistered, NRPT rule removed, and app-tagged `hosts` entries cleaned up (Engine should support an `--uninstall-service` path that also does this cleanup, not just `DeleteService`)
- [ ] Appytizer UI's own "run at startup" registry entry (existing template logic) is a **separate, per-user** concern from the service — don't conflate the two in the installer; the installer only needs to handle the Engine service and initial file placement, the app itself manages its own Run-key autostart from Settings
- [x] Add Start Menu shortcut for the Appytizer UI; installer does not need a shortcut for the headless Engine
- [ ] Since this is open source and the installer will be unsigned initially, expect SmartScreen warnings — note this in the repo's README/release notes rather than trying to solve it in v1 (code signing is a real cost, revisit if the project gets traction)

---

## 10. Milestone Order (recap — work top to bottom)

> Implementation snapshot (2026-08-03): the buildable vertical slice is complete and verified. Remaining unchecked items are intentionally not claimed: upstream DNS forwarding, deeper registry/PATH detection, periodic push telemetry, nginx graceful reload orchestration, version-selector UI, PNG sidebar artwork, and UI autostart registry wiring.

- [ ] Phase 0: Repo scaffolding + CMake/vcpkg (§4, §3)
- [ ] Phase 1: `/common` — config schema, IPC message structs, `ServiceProvider` interface (§5.2, §5.9, §6)
- [ ] Phase 2: Engine core — service host boilerplate, Job Objects, RAM polling (§5.1, §5.3, §5.4)
- [ ] Phase 3: DNS server + NRPT scoping + hosts mirroring (§5.5, §5.6, §5.7)
- [ ] Phase 4: `NginxProvider` + `PhpProvider` + config generation (§5.7 nginx bit, §5.2)
- [ ] Phase 5: `MySqlProvider` / `PostgresProvider` / `MongoProvider` (§5.2)
- [ ] Phase 6: IPC server (Engine) + client (Appytizer UI), FLTK-thread-safe (§6)
- [ ] Phase 7: Visual restyle of existing template — colors, fonts, radii, sidebar layout (§7.1, §7.2)
- [ ] Phase 8: Dashboard view + `ServiceCard` (§7.3)
- [ ] Phase 9: Services view + version selector (§7.3)
- [ ] Phase 10: Sites view + `SiteRow` + Copy URL (§7.3)
- [ ] Phase 11: Settings view (run-minimized, extension chooser, root folder picker) (§7.3)
- [ ] Phase 12: Tray context menu wiring — Display UI / Stop all and exit / Exit (§7.4)
- [ ] Phase 13: `installer.iss` (§9)
- [ ] Phase 14: Pass over logging/error handling consistency + unit tests for DNS parsing and detection utils (§8, §3)

---

## 11. Roadmap (explicitly out of scope for this plan)

- Bundling PHP/nginx/phpMyAdmin inside the installer
- Per-site version pinning
- Python/Node.js `ServiceProvider`s (architecture supports it, just not built yet — adding one should mean writing one new provider file and registering it, nothing else)
- RAM usage sparkline history (SQLite table is scaffolded for it in §5.9, UI not built)
