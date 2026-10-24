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

  TRIAGE ORDER for "flaky timeout" / "test hangs with zero output" -- ENVIRONMENT FIRST,
  CODE SECOND:
    1. run the failing .exe directly, then immediately run it again;
    2. if both pass, it was the first-execution antivirus scan (measured on this host:
       a freshly linked or freshly copied exe took >70s and printed nothing at all --
       not even main() -- then 0.9s once the scanner had seen it);
    3. only after that, suspect a regression.
  ctest TIMEOUT is 180s for this reason. A guard that trips on the healthy path (fresh
  build directory -> ctest, which is what CI does every day) is noise, not a guard.

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

# ---------------------------------------------------------------------------
# 4. Compile gate: every generated TU must compile with /W4 /WX
# ---------------------------------------------------------------------------
function Invoke-CompileGate([string] $label, [string] $inputPattern) {
  Write-Step "Stage 4/4: compile gate [$label]"
  $outDir = Join-Path $ciDir $label
  $objDir = Join-Path $outDir 'obj'
  & $cmake -E make_directory $objDir | Out-Null

  Push-Location $rootDir
  try {
    & $cmake -E env "PYTHONPATH=$codegenDir" $python -m qzcodegen `
        --input $inputPattern --out $outDir --cpp
    if ($LASTEXITCODE -ne 0) { Stop-WithError "codegen failed for $inputPattern" }
  } finally {
    Pop-Location
  }

  $sources = @(Get-ChildItem -LiteralPath $outDir -Filter *.cpp -File)
  if ($sources.Count -eq 0) { Stop-WithError "no .cpp produced for $inputPattern" }

  $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
  foreach ($source in $sources) {
    # /Fd is NOT optional here: without it cl writes vc140.pdb into the CURRENT directory,
    # which is the repo root (Push-Location above) -- every CI run would drop a stray
    # binary artifact next to CMakeLists.txt. Same reason /Fo points into $objDir.
    & $cl /nologo /c /std:c++17 /utf-8 /W4 /WX /permissive- /EHsc /MD `
        /I (Join-Path $rootDir 'src') /I $outDir `
        /Fd"$objDir\vc.pdb" /Fo"$objDir\" $source.FullName
    if ($LASTEXITCODE -ne 0) {
      Stop-WithError "generated source does not compile: $($source.Name)"
    }
  }
  $stopwatch.Stop()
  Write-Host ("    {0} TU(s) compiled in {1:N2}s" -f $sources.Count, $stopwatch.Elapsed.TotalSeconds)
  $script:tuCompiled += $sources.Count
}

$script:tuCompiled = 0
Invoke-CompileGate 'idl' 'idl/*.qz'
Invoke-CompileGate 'fixtures' 'codegen/tests/fixtures/*.qz'

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
