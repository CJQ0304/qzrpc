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
#
# ALWAYS configure, even when the build directory already exists and only `build` was
# asked for. `cmake --build` alone is not enough to keep a build honest: tests\
# CMakeLists.txt skips a source it cannot find (`if (NOT EXISTS ...)`), so a configure
# that happened while a test file was still being written bakes that skip in. Ninja does
# not re-run configure afterwards (no CMakeLists.txt mtime change), and ctest then prints
# "100% tests passed, 0 tests failed out of 9" while 4 cases are simply not there.
# Step 5 below verifies the COUNT for exactly this reason -- see the guard there.
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
  # -------------------------------------------------------------------------
  # Guard: every tests\test_*.cpp on disk must be a registered ctest case.
  #
  # This is not a hypothetical. A configure that ran while a test file was still
  # being written skips it (tests\CMakeLists.txt: `if (NOT EXISTS ...) continue()`),
  # and `cmake --build` will never re-run configure afterwards. The result is a
  # perfectly green run that is missing cases:
  #   100% tests passed, 0 tests failed out of 9     <- 4 cases / 8035 assertions gone
  # Nothing errors, nothing warns, and it reads healthier than the real thing. So the
  # count is verified instead of the verdict being trusted -- "all green" must also
  # mean "all present".
  # -------------------------------------------------------------------------
  $testListing = & ctest --test-dir $buildPath -C $Config -N
  if ($LASTEXITCODE -ne 0) { Stop-WithError "ctest -N failed (exit code $LASTEXITCODE)" }
  $registeredCpp = @()
  foreach ($line in $testListing) {
    $nameMatch = [regex]::Match($line, '^\s*Test\s+#\d+:\s+(\S+)')
    # codegen_python / interop_protobuf come from other CMakeLists and have no
    # tests\test_*.cpp behind them, so only test_* names are compared here.
    if ($nameMatch.Success -and $nameMatch.Groups[1].Value -like 'test_*') {
      $registeredCpp += $nameMatch.Groups[1].Value
    }
  }
  $sourcesOnDisk = @(Get-ChildItem -LiteralPath (Join-Path $rootDir 'tests') -Filter 'test_*.cpp' -File)
  $missingTests = @($sourcesOnDisk | Where-Object { $registeredCpp -notcontains $_.BaseName })
  if ($missingTests.Count -gt 0) {
    $names = ($missingTests | ForEach-Object { $_.Name }) -join ', '
    Stop-WithError ("test source(s) exist but are not registered with ctest: {0} (a stale configure skips files that did not exist when it ran; delete the build dir or fix tests\CMakeLists.txt)" -f $names)
  }
  Write-Host ("    ctest knows {0} C++ case(s), tests\ has {1} test_*.cpp" -f `
    $registeredCpp.Count, $sourcesOnDisk.Count)

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
