#Requires -Version 5.1
<#
  Qingzhou RPC - CI pipeline (Windows / MSVC).

  Stages:
    1. build + unit tests      : delegates to scripts\build.ps1 -Checks
                                 (/WX on, ctest = C++ tests + codegen Python tests
                                 + interop_protobuf, which self-skips when this machine
                                 has no protobuf/protoc -- see tools/interop/CMakeLists.txt)
    2. idl contract            : qzcodegen --check on idl/*.qz (no output written)
    3. generated/ is in sync   : regenerate into build\ci\gen_fresh and byte-compare
                                 with the committed generated/ (SHA256)
    4. codegen compile gate    : regenerate idl/*.qz and codegen/tests/fixtures/*.qz,
                                 then `cl /c` EVERY produced .cpp (no link, ~1.6s/TU)

  Why stage 4 exists: the generator can emit code that compiles fine on idl/echo.qz
  (8 field shapes) yet is wrong for other shapes -- a packed-field tag emitted with the
  wrong wire type compiles, passes unit tests, and only breaks when the peer decodes it.
  kitchen_sink.qz covers 11 scalars x 4 labels, [packed=false], empty messages, nested
  types, duplicate method names and INT64_MIN/UINT64_MAX defaults.

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts\ci.ps1
    powershell -ExecutionPolicy Bypass -File scripts\ci.ps1 -SkipTests
    powershell -ExecutionPolicy Bypass -File scripts\ci.ps1 -SkipBuild   (gates 2-4 only)
    powershell -ExecutionPolicy Bypass -File scripts\ci.ps1 -Config Debug

  -SkipBuild exists because a teammate mid-edit can leave the tree un-compilable while
  the IDL / generator / committed-artifact gates are still fully checkable -- those
  three gates are exactly what a codegen-side change needs to be validated against.

  Stage 1 always runs a full `cmake -S -B` configure before the build (scripts\build.ps1
  step 3). That is deliberate and must stay: `cmake --build` on an existing build tree does
  NOT pick up a newly added test file, because tests\CMakeLists.txt skips sources it cannot
  find at configure time and ninja sees no dependency change afterwards. A "100% tests
  passed, 0 tests failed out of 9" was seen exactly that way while 4 cases were missing.
  build.ps1 now also cross-checks tests\test_*.cpp against `ctest -N` and fails on a gap.

  About the ctest TIMEOUT (180s): it is a pure guardrail, NOT a performance budget and
  NOT an environment allowance. The whole suite finishes in under 5 seconds on this host,
  so 180s can only ever fire on a real hang. Every case prints "== TestXxx" and flushes
  before it runs, so a hang names itself in the last line before ctest's timer kills it.

  The one "Timeout 60.03s" seen on this project was NOT an environment problem. It was a
  lost wakeup in EventLoop::Loop: the loop thread wiped `quit_` on entry, erasing a Quit()
  that had arrived in the window between thread creation and the first line of Loop() --
  the thread then never exited and join() hung forever (fixed in 12617e8, locked by
  tests/test_event_loop.cpp). The lesson is worth keeping: a wrong environment attribution
  ("it is just the antivirus / just rerun it") is exactly how a real concurrency bug
  survives a retry. Rerun-to-green proves nothing -- it would also have "worked" here.

  DO NOT PIPE THIS SCRIPT WITHOUT DRAINING IT. Measured the hard way: running
    powershell -File scripts\ci.ps1 *>&1 | Tee-Object -FilePath build\ci.log
  hung for ten minutes at 55/74 with cl.exe sitting at 0.1s CPU. Nothing was wrong with
  the tree -- the compile was blocked writing to a pipe whose consumer had stopped
  reading. cl emits a lot of text with /showIncludes, so a full pipe is reached quickly.
  Redirect straight to a file (`... *> build\ci.log`) or run it in a console; if you need
  a pipeline, make sure the consumer keeps reading.

  OFFLINE: every stage runs with no network access. The toolchain is already on disk
  (msvc_env.cmd), the generator is local Python, and the only optional external tool
  (protoc, used by the interop_protobuf ctest case) self-skips with exit code 2 when it
  is missing. Nothing here downloads anything -- important because this build host sits
  behind a proxy (HTTP_PROXY/http_proxy are both set), and a CI gate that hangs on a
  proxy handshake is worse than one that fails.

  NOTE: keep this file ASCII-only (cmd.exe parses it as code page 936 in some setups).
#>
[CmdletBinding()]
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
  [string] $Config = 'Release',

  [switch] $SkipTests,

  # Run only the codegen/artifact gates (stages 2-4). Useful while a teammate is
  # mid-edit and the tree does not compile, or when only IDL/generated changed.
  [switch] $SkipBuild,

  [string] $BuildDir = 'build'
)

$ErrorActionPreference = 'Stop'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir    = Split-Path -Parent $scriptDir
$buildPath  = Join-Path $rootDir $BuildDir
$ciDir      = Join-Path $buildPath 'ci'
$codegenDir = Join-Path $rootDir 'codegen'

# Pin the interpreter to an ABSOLUTE path instead of the bare name "python".
# Reason: a bare name is resolved against PATH at RUN time, and PATH differs between
# PowerShell and cmd/Git Bash on the same machine. That is how one ctest case ended up
# "Passed in PowerShell, ***Skipped in Git Bash" -- a difference far more dangerous than
# a red test, because it gets written off as environment noise.
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
if ($null -eq $pythonCommand) {
  Write-Host "ERROR: python not found on PATH (required by the codegen gates 2-4)" -ForegroundColor Red
  exit 1
}
$python = $pythonCommand.Source

function Write-Step([string] $message) {
  Write-Host "==> $message" -ForegroundColor Cyan
}

function Stop-WithError([string] $message) {
  Write-Host "ERROR: $message" -ForegroundColor Red
  exit 1
}

# ---------------------------------------------------------------------------
# 0. Toolchain
# ---------------------------------------------------------------------------
Write-Step "Importing MSVC environment"
$msvcEnvCmd = Join-Path $scriptDir 'msvc_env.cmd'
if (-not (Test-Path -LiteralPath $msvcEnvCmd)) { Stop-WithError "missing $msvcEnvCmd" }
$envDump = & $env:ComSpec /c "`"$msvcEnvCmd`" >nul 2>&1 && set"
foreach ($line in $envDump) {
  $separator = $line.IndexOf('=')
  if ($separator -le 0) { continue }
  Set-Item -Path ("env:" + $line.Substring(0, $separator)) -Value $line.Substring($separator + 1)
}
foreach ($tool in @('cl.exe', 'cmake.exe', 'ctest.exe')) {
  if ($null -eq (Get-Command $tool -ErrorAction SilentlyContinue)) {
    Stop-WithError "$tool not found on PATH after importing msvc_env.cmd"
  }
}
$cmake = (Get-Command cmake.exe).Source
$cl    = (Get-Command cl.exe).Source

if (Test-Path -LiteralPath $ciDir) { Remove-Item -LiteralPath $ciDir -Recurse -Force }
& $cmake -E make_directory $ciDir | Out-Null

# ---------------------------------------------------------------------------
# 1. Build + unit tests (single definition of the build lives in build.ps1)
# ---------------------------------------------------------------------------
if ($SkipBuild) {
  Write-Step "Stage 1/4: build + ctest -- SKIPPED (-SkipBuild)"
} else {
  Write-Step "Stage 1/4: build + ctest"
  $buildArgs = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $scriptDir 'build.ps1'),
    '-Config', $Config,
    '-Checks',
    '-BuildDir', $BuildDir
  )
  if ($SkipTests) { $buildArgs += '-NoTest' }
  & powershell @buildArgs
  if ($LASTEXITCODE -ne 0) { Stop-WithError "build/tests failed (exit code $LASTEXITCODE)" }
}

# ---------------------------------------------------------------------------
# 2. IDL contract: --check writes nothing, exit code tells whether the IDL is valid
# ---------------------------------------------------------------------------
Write-Step "Stage 2/4: qzcodegen --check on idl/*.qz"
Push-Location $rootDir
try {
  & $cmake -E env "PYTHONPATH=$codegenDir" $python -m qzcodegen --input "idl/*.qz" --check
  if ($LASTEXITCODE -ne 0) { Stop-WithError "codegen --check rejected idl/*.qz" }
} finally {
  Pop-Location
}

# ---------------------------------------------------------------------------
# 3. Committed generated/ must match what the generator produces today
# ---------------------------------------------------------------------------
Write-Step "Stage 3/4: generated/ is in sync with idl/"
$freshDir = Join-Path $ciDir 'gen_fresh'
& $cmake -E make_directory $freshDir | Out-Null
Push-Location $rootDir
try {
  & $cmake -E env "PYTHONPATH=$codegenDir" $python -m qzcodegen `
      --input "idl/*.qz" --out $freshDir --cpp
  if ($LASTEXITCODE -ne 0) { Stop-WithError "codegen failed while regenerating idl/*.qz" }
} finally {
  Pop-Location
}

function Compare-Directory([string] $left, [string] $right) {
  $leftFiles = @(Get-ChildItem -LiteralPath $left -File | Sort-Object Name)
  $rightFiles = @(Get-ChildItem -LiteralPath $right -File | Sort-Object Name)
  if ($leftFiles.Count -ne $rightFiles.Count) {
    return "file count differs: $($leftFiles.Count) vs $($rightFiles.Count)"
  }
  for ($i = 0; $i -lt $leftFiles.Count; $i++) {
    if ($leftFiles[$i].Name -ne $rightFiles[$i].Name) {
      return "file name differs: $($leftFiles[$i].Name) vs $($rightFiles[$i].Name)"
    }
    $hashLeft  = (Get-FileHash -LiteralPath $leftFiles[$i].FullName -Algorithm SHA256).Hash
    $hashRight = (Get-FileHash -LiteralPath $rightFiles[$i].FullName -Algorithm SHA256).Hash
    if ($hashLeft -ne $hashRight) {
      return "content differs: $($leftFiles[$i].Name) (generator output changed; commit generated/ or fix the generator drift)"
    }
  }
  return ''
}

$mismatch = Compare-Directory $freshDir (Join-Path $rootDir 'generated')
if ($mismatch -ne '') {
  Stop-WithError "generated/ is out of sync with idl/: $mismatch"
}
Write-Host "    generated/ matches the generator output"

# Run a native command and hand back everything it wrote on BOTH streams.
#
# Why this exists: the one warning this project tolerates (the explicit method_id pin in
# kitchen_sink.qz) is printed by the PYTHON generator on **stderr**, and PowerShell 5.1
# turns native stderr into ErrorRecord objects. An assertion that only looked at stdout
# would never see it -- the warning would stay visible to a human reading the log and to
# nothing else, which is exactly the state we were in.
function Invoke-Captured([string] $exe, [string[]] $arguments) {
  # This script sets $ErrorActionPreference = 'Stop'. Under that setting a native command
  # writing to stderr does not merely produce an ErrorRecord -- it THROWS, so the warning
  # we are trying to count would abort the gate instead of being captured. Scope the
  # preference down to the call and restore it.
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $lines = @(& $exe @arguments *>&1 | ForEach-Object { [string]$_ })
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previous
  }
  return New-Object psobject -Property @{ ExitCode = $code; Lines = $lines }
}

# ---------------------------------------------------------------------------
# 4. Compile gate: every generated TU must compile with /W4 /WX
#
# $expectWarnings / $expectWarningPattern make tolerated warnings EXPLICIT:
#   * exactly that many warnings may appear;
#   * each one must match the pattern.
# Anything else is red. Disabling the warning (/wd) or deleting the fixture would also
# make the log quiet -- and would hide the NEXT warning in that file forever. An expected
# warning has to be declared and counted, not tolerated as background noise.
# ---------------------------------------------------------------------------
function Invoke-CompileGate([string] $label, [string] $inputPattern,
                             [int] $expectWarnings, [string] $expectWarningPattern) {
  Write-Step "Stage 4/4: compile gate [$label]"
  $outDir = Join-Path $ciDir $label
  $objDir = Join-Path $outDir 'obj'
  & $cmake -E make_directory $objDir | Out-Null

  $transcript = @()
  Push-Location $rootDir
  try {
    $generate = Invoke-Captured $cmake @('-E', 'env', "PYTHONPATH=$codegenDir", $python,
        '-m', 'qzcodegen', '--input', $inputPattern, '--out', $outDir, '--cpp')
  } finally {
    Pop-Location
  }
  $transcript += $generate.Lines
  if ($generate.ExitCode -ne 0) {
    $transcript | ForEach-Object { Write-Host $_ }
    Stop-WithError "codegen failed for $inputPattern"
  }

  $sources = @(Get-ChildItem -LiteralPath $outDir -Filter *.cpp -File)
  if ($sources.Count -eq 0) { Stop-WithError "no .cpp produced for $inputPattern" }

  $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  foreach ($source in $sources) {
    # /Fd is NOT optional here: without it cl writes vc140.pdb into the CURRENT directory,
    # which is the repo root (Push-Location above) -- every CI run would drop a stray
    # binary artifact next to CMakeLists.txt. Same reason /Fo points into $objDir.
    $compile = Invoke-Captured $cl @('/nologo', '/c', '/std:c++17', '/utf-8', '/W4', '/WX',
        '/permissive-', '/EHsc', '/MD',
        '/I', (Join-Path $rootDir 'src'), '/I', $outDir,
        "/Fd$objDir\vc.pdb", "/Fo$objDir\", $source.FullName)
    $transcript += $compile.Lines
    if ($compile.ExitCode -ne 0) {
      $transcript | ForEach-Object { Write-Host $_ }
      Stop-WithError "generated source does not compile: $($source.Name)"
    }
  }
  $stopwatch.Stop()

  # Replay what was captured: the log must still show what actually ran.
  $transcript | ForEach-Object { Write-Host $_ }

  # The negative lookahead matters: qzcodegen ends with a summary line
  # "2 file(s) generated, 2 updated, 0 warning(s)", which contains the word "warning" and
  # would otherwise be counted as one. Caught by running the gate, not by reading it.
  $warnings = @($transcript | Where-Object { $_ -match '(?i)\bwarning\b(?!\(s\))' })
  if ($warnings.Count -ne $expectWarnings) {
    $detail = ("compile gate [{0}]: expected exactly {1} warning(s), saw {2}. " -f `
        $label, $expectWarnings, $warnings.Count)
    $detail += "A new warning is a regression, not noise -- fix it, or declare it here with a pattern."
    Stop-WithError $detail
  }
  foreach ($warning in $warnings) {
    if ($warning -notmatch $expectWarningPattern) {
      Stop-WithError ("compile gate [{0}]: undeclared warning (does not match '{1}'): {2}" -f `
          $label, $expectWarningPattern, $warning)
    }
  }

  Write-Host ("    {0} TU(s) compiled in {1:N2}s, {2} declared warning(s)" -f `
      $sources.Count, $stopwatch.Elapsed.TotalSeconds, $warnings.Count)
  $script:tuCompiled += $sources.Count
}

$script:tuCompiled = 0
Invoke-CompileGate 'idl' 'idl/*.qz' 0 ''
# kitchen_sink.qz pins one method_id on purpose; the generator warns about it. Exactly one
# warning, and it must be that one.
Invoke-CompileGate 'fixtures' 'codegen/tests/fixtures/*.qz' 1 'explicit method_id'

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Step "CI OK"
Write-Host ("    config      : {0}" -f $Config)
if ($SkipBuild) {
  Write-Host "    build+tests : skipped (-SkipBuild)"
} elseif ($SkipTests) {
  Write-Host "    build+tests : build only (-SkipTests)"
} else {
  Write-Host ("    build+tests : ctest (C++ unit + codegen Python + interop_protobuf) [{0}]" -f $Config)
}
Write-Host "    gates       : qzcodegen --check, generated/ SHA256 sync, /W4 /WX compile gate"
Write-Host ("    codegen TUs : {0} (idl + fixtures)" -f $script:tuCompiled)
Write-Host ("    python      : {0}" -f $python)
exit 0
