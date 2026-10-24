#Requires -Version 5.1
<#
  Qingzhou RPC - Windows build script (MSVC + CMake + Ninja).

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Debug -NoTest
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Checks

  -Checks turns on the CI gates: /WX (QZRPC_WERROR) and the codegen compile gate
  (QZRPC_CHECK_CODEGEN, compiles codegen/tests/fixtures/kitchen_sink.qz output).

  Why not vcvars64.bat: reg.exe is blocked by endpoint security on this host, so
  vcvars cannot locate the Windows SDK and cl.exe fails with "cannot open include
  file: stdio.h". scripts\msvc_env.cmd sets INCLUDE/LIB/PATH by hand.

  That .cmd file stays the single source of truth for the toolchain location:
  this script does NOT duplicate the paths, it imports the environment of a
  cmd.exe child process (a .cmd run from PowerShell cannot modify its parent's
  environment, so `& msvc_env.cmd` alone would be a no-op).

  NOTE: keep this file ASCII-only. It is also invoked from cmd.exe in some CI
  setups, and cmd parses .ps1 comments inside here-strings as the OEM code page.
#>
[CmdletBinding()]
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
  [string] $Config = 'Release',

  [switch] $Clean,
  [switch] $NoTest,

  # CI gates: /WX + codegen compile gate. Off by default so that a developer's
  # build is never blocked by a warning introduced in someone else's module.
  [switch] $Checks,

  [string] $BuildDir = 'build'
)

$ErrorActionPreference = 'Stop'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir    = Split-Path -Parent $scriptDir
$msvcEnvCmd = Join-Path $scriptDir 'msvc_env.cmd'
$buildPath  = Join-Path $rootDir $BuildDir

function Write-Step([string] $message) {
  Write-Host "==> $message" -ForegroundColor Cyan
}

function Stop-WithError([string] $message) {
  Write-Host "ERROR: $message" -ForegroundColor Red
  exit 1
}

# ---------------------------------------------------------------------------
# 1. Import the MSVC environment
# ---------------------------------------------------------------------------
Write-Step "Importing MSVC environment (scripts\msvc_env.cmd)"
if (-not (Test-Path -LiteralPath $msvcEnvCmd)) {
  Stop-WithError "missing $msvcEnvCmd"
}

$envDump = & $env:ComSpec /c "`"$msvcEnvCmd`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
  Stop-WithError "msvc_env.cmd failed with exit code $LASTEXITCODE"
}
foreach ($line in $envDump) {
  $separator = $line.IndexOf('=')
  # Lines such as "=C:=D:\path" (cmd's per-drive current directory) start with
  # '=', so they are skipped by the -le 0 test.
  if ($separator -le 0) { continue }
  $name  = $line.Substring(0, $separator)
  $value = $line.Substring($separator + 1)
  Set-Item -Path ("env:" + $name) -Value $value
}

foreach ($tool in @('cl.exe', 'cmake.exe', 'ninja.exe', 'ctest.exe')) {
  $command = Get-Command $tool -ErrorAction SilentlyContinue
  if ($null -eq $command) {
    Stop-WithError "$tool not found on PATH after importing msvc_env.cmd"
  }
}
Write-Step ("cl: {0}" -f ((Get-Command cl.exe).Source))

# ---------------------------------------------------------------------------
# 2. Optional clean
# ---------------------------------------------------------------------------
if ($Clean) {
  if (Test-Path -LiteralPath $buildPath) {
    Write-Step "Removing $buildPath"
    Remove-Item -LiteralPath $buildPath -Recurse -Force
  }
}

# ---------------------------------------------------------------------------
# 3. Configure
# ---------------------------------------------------------------------------
Write-Step "Configuring ($Config) -> $buildPath"
$configureArgs = @(
  '-S', $rootDir,
  '-B', $buildPath,
  '-G', 'Ninja',
  "-DCMAKE_BUILD_TYPE=$Config"
)
if ($Checks) {
  Write-Step "CI gates enabled: /WX + codegen compile gate"
  $configureArgs += '-DQZRPC_WERROR=ON'
  $configureArgs += '-DQZRPC_CHECK_CODEGEN=ON'
}
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
  Stop-WithError "cmake configure failed (exit code $LASTEXITCODE)"
}

# ---------------------------------------------------------------------------
# 4. Build
# ---------------------------------------------------------------------------
Write-Step "Building"
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
& cmake --build $buildPath --parallel
if ($LASTEXITCODE -ne 0) {
  Stop-WithError "build failed (exit code $LASTEXITCODE)"
}
$stopwatch.Stop()

# ---------------------------------------------------------------------------
# 5. Test
# ---------------------------------------------------------------------------
if (-not $NoTest) {
  Write-Step "Running ctest"
  & ctest --test-dir $buildPath -C $Config --output-on-failure
  if ($LASTEXITCODE -ne 0) {
    Stop-WithError "ctest reported failures (exit code $LASTEXITCODE)"
  }
}

# ---------------------------------------------------------------------------
# 6. Summary
# ---------------------------------------------------------------------------
Write-Step "Artifacts"
$artifacts = Get-ChildItem -LiteralPath $buildPath -Recurse -File |
             Where-Object { $_.Extension -in '.exe', '.lib', '.a', '.so' }
foreach ($artifact in $artifacts) {
  Write-Host ("    {0,-36} {1,10:N0} bytes" -f $artifact.Name, $artifact.Length)
}
Write-Host ("    build time: {0:N2}s" -f $stopwatch.Elapsed.TotalSeconds)

Write-Step "OK"
exit 0
