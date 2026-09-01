[CmdletBinding()]
param(
    [string]$NginxVersion = "1.31.3",
    [string]$PhpVersion = "8.5.9",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\")).Path
$outputRoot = Join-Path $repoRoot "installer\Output"
$nginxRoot = Join-Path $outputRoot "nginx-$NginxVersion"
$phpRoot = Join-Path $outputRoot "php85"

if ($NginxVersion -ne "1.31.3") {
    throw "installer\installer.iss currently packages nginx-1.31.3; update that definition before using another nginx version."
}

function Invoke-RequiredCommand {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string[]]$Arguments
    )

    & $Name @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE."
    }
}

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw "CMake was not found on PATH. Install CMake 3.21 or newer."
}
if ([string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
    throw "VCPKG_ROOT is not set. Point it at your vcpkg checkout before building."
}

$innoCandidates = @(
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$innoCompiler = $innoCandidates | Select-Object -First 1
if (-not $innoCompiler) {
    throw "Inno Setup 6 was not found. Install it from https://jrsoftware.org/isinfo.php and run this script again."
}

Push-Location $repoRoot
try {
    Write-Host "Configuring and building Release..."
    Invoke-RequiredCommand cmake.exe @("--preset", "windows-release")
    Invoke-RequiredCommand cmake.exe @("--build", "--preset", "windows-release")

    if (-not $SkipTests) {
        Write-Host "Running tests..."
        Invoke-RequiredCommand ctest.exe @("--preset", "windows-release", "--output-on-failure", "--timeout", "60")
    }

    New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) "appytizer-runtime-$PID"
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $nginxRoot "nginx.exe"))) {
            $archive = Join-Path $tempRoot "nginx.zip"
            Write-Host "Downloading nginx $NginxVersion..."
            Invoke-WebRequest -Uri "https://nginx.org/download/nginx-$NginxVersion.zip" -OutFile $archive
            Expand-Archive -LiteralPath $archive -DestinationPath $outputRoot -Force
        }
        if (-not (Test-Path -LiteralPath (Join-Path $nginxRoot "nginx.exe"))) {
            throw "The nginx archive did not contain nginx-$NginxVersion\nginx.exe."
        }

        if (-not (Test-Path -LiteralPath (Join-Path $phpRoot "php-cgi.exe"))) {
            $archive = Join-Path $tempRoot "php.zip"
            Write-Host "Downloading PHP $PhpVersion..."
            Invoke-WebRequest -Uri "https://windows.php.net/downloads/releases/archives/php-$PhpVersion-nts-Win32-vs17-x64.zip" -OutFile $archive
            New-Item -ItemType Directory -Force -Path $phpRoot | Out-Null
            Expand-Archive -LiteralPath $archive -DestinationPath $phpRoot -Force
        }
        if (-not (Test-Path -LiteralPath (Join-Path $phpRoot "php-cgi.exe"))) {
            throw "The PHP archive did not contain php-cgi.exe."
        }

        $bundledIni = Join-Path $repoRoot "installer\php.ini"
        if (-not (Test-Path -LiteralPath $bundledIni)) {
            throw "Bundled PHP configuration was not found at $bundledIni."
        }
        Copy-Item -LiteralPath $bundledIni -Destination (Join-Path $phpRoot "php.ini") -Force
    }
    finally {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host "Compiling installer with $innoCompiler..."
    Invoke-RequiredCommand $innoCompiler @((Join-Path $repoRoot "installer\installer.iss"))
    Write-Host "Created $outputRoot\AppytizerSetup.exe"
}
finally {
    Pop-Location
}
