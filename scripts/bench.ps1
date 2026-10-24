#Requires -Version 5.1
<#
  Qingzhou RPC - benchmark runner (docs/00-file-map.md B-08).

  Runs `echo_client --bench N T` over a matrix of thread counts, repeats each point
  several times, and prints a Markdown table that can be pasted straight into the
  README's performance section (the `<!-- BENCH -->` marker).

  Why a matrix and repeats instead of one run:
    * one number from one run is not a measurement, it is an anecdote -- the first run
      pays page faults and TCP slow-start, the last one may run into another process
      stealing the CPU;
    * QPS vs concurrency is the only curve that shows whether the server scales or
      saturates, and "saturated at 1 thread already" is a very different finding from
      "keeps scaling to 8";
    * so: warmup, then N rounds per point, and report the MEDIAN with min..max next to
      it. If min and max are far apart, the machine was noisy -- that must be visible,
      not averaged away.

  Also writes the machine profile into the output: a QPS number without the CPU model
  and core count is not reproducible, so it is not evidence.

  READ THIS BEFORE QUOTING THE NUMBERS -- what this sweep does NOT measure:
    example\echo_client.cpp sets `connections_per_endpoint = 1` (single endpoint, and also
    the multi-endpoint demo path). So every client thread shares ONE connection per
    server, and request/response on that connection is one-at-a-time. The sweep therefore
    measures `one connection round-trip throughput`, NOT server capacity: adding threads
    only adds queueing (measured: 1 thread 14.9k QPS / p50 54us vs 4 threads 13.4k QPS /
    p50 235us -- flat-to-worse, exactly what a single serialized connection predicts).
    To measure the server you need either -Connections (a flag that does not exist yet on
    echo_client) or N servers x 1 connection each. Until then, treat this table as a
    per-connection latency/throughput baseline and say so wherever it is quoted.

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts\bench.ps1
    powershell -ExecutionPolicy Bypass -File scripts\bench.ps1 -Threads 1,2,4,8,16 -Requests 5000 -Rounds 5
    powershell -ExecutionPolicy Bypass -File scripts\bench.ps1 -SkipBuild -Output bench.md

  Exit code: 0 = every round ran and parsed, 1 = a round failed or its output did not parse.

  NOTE: keep this file ASCII-only (cmd.exe parses it as code page 936 in some setups).
#>
[CmdletBinding()]
param(
  [int]    $Port        = 16161,

  # 0 disables the built-in /metrics endpoint (the server then logs no metrics).
  [int]    $MetricsPort = 9101,

  # Comma-separated thread counts, e.g. "1,2,4,8".
  #
  # Taken as a STRING and split by hand, for a measured reason: `-Threads 1,4` passed
  # through `powershell -File script.ps1 -Threads 1,4` arrives as ONE argument whose text
  # is "1,4", and casting that to [int[]] does not split it -- it produced a single
  # element 14 (the comma silently swallowed), so the sweep ran a bogus 14-thread point.
  # Parsing the string here makes the CLI behave the way it reads.
  [string] $Threads     = '1,2,4,8',

  # Requests PER THREAD per round (that is how echo_client --bench N T defines N).
  [int]    $Requests    = 2000,

  [int]    $Rounds      = 3,

  # Warmup rounds: discarded, they exist to page in code and warm the connection.
  [int]    $Warmup      = 200,

  [string] $BuildDir    = 'build',

  # Markdown output path; empty = <BuildDir>\bench.md
  [string] $Output      = '',

  [switch] $SkipBuild,

  # Leave the server running afterwards (for a second run against a warm process).
  [switch] $KeepServer
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir   = Split-Path -Parent $scriptDir
$buildPath = Join-Path $rootDir $BuildDir
$serverExe = Join-Path $buildPath 'example\echo_server.exe'
$clientExe = Join-Path $buildPath 'example\echo_client.exe'
if ($Output -eq '') { $Output = Join-Path $buildPath 'bench.md' }

function Write-Step([string] $message) { Write-Host ("==> {0}" -f $message) -ForegroundColor Cyan }
function Stop-WithError([string] $message) {
  Write-Host ("ERROR: {0}" -f $message) -ForegroundColor Red
  exit 1
}

# "1,2,4,8" -> @(1,2,4,8). Rejects empty items and non-positive values early: a 0-thread
# point would make echo_client fall back to its own default and quietly measure the wrong
# thing.
function ConvertTo-ThreadList([string] $text) {
  $list = @()
  foreach ($part in ($text -split ',')) {
    $trimmed = $part.Trim()
    if ($trimmed -eq '') { continue }
    $value = 0
    if (-not [int]::TryParse($trimmed, [ref]$value) -or $value -le 0 -or $value -gt 256) {
      Stop-WithError ("invalid thread count '{0}' (expect 1..256 in a comma-separated list)" -f $trimmed)
    }
    $list += $value
  }
  if ($list.Count -eq 0) { Stop-WithError 'no thread count given (-Threads 1,2,4,8)' }
  return $list
}

$threadList = ConvertTo-ThreadList $Threads

# ---------------------------------------------------------------------------
# 0. Toolchain
# ---------------------------------------------------------------------------
Write-Step 'Importing MSVC environment'
$msvcEnvCmd = Join-Path $scriptDir 'msvc_env.cmd'
if (-not (Test-Path -LiteralPath $msvcEnvCmd)) { Stop-WithError "missing $msvcEnvCmd" }
$envDump = & $env:ComSpec /c "`"$msvcEnvCmd`" >nul 2>&1 && set"
foreach ($line in $envDump) {
  $separator = $line.IndexOf('=')
  if ($separator -le 0) { continue }
  Set-Item -Path ("env:" + $line.Substring(0, $separator)) -Value $line.Substring($separator + 1)
}
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($null -eq $cmake) { Stop-WithError 'cmake.exe not found after importing msvc_env.cmd' }

if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'CMakeCache.txt'))) {
  Stop-WithError "no configured build in '$BuildDir' (run scripts\build.ps1 first)"
}
if (-not $SkipBuild) {
  Write-Step 'Building echo_server + echo_client'
  & $cmake.Source --build $buildPath --parallel --target echo_server echo_client
  if ($LASTEXITCODE -ne 0) { Stop-WithError "build failed (exit code $LASTEXITCODE)" }
}
foreach ($exe in @($serverExe, $clientExe)) {
  if (-not (Test-Path -LiteralPath $exe)) { Stop-WithError "$exe not found" }
}

# ---------------------------------------------------------------------------
# Helpers (same shape as scripts\run_demo.ps1 -- see the comments there)
#
# Start-Process is avoided on purpose: it rebuilds the child environment in a
# case-INSENSITIVE dictionary and throws "an item with the same key has already been
# added" on hosts that have both HTTPS_PROXY and https_proxy. ProcessStartInfo just
# inherits the OS environment block.
# ---------------------------------------------------------------------------
function Start-ServerProcess([string] $exePath, [string[]] $arguments) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName        = $exePath
  $psi.Arguments       = ($arguments -join ' ')
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow  = $false
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $psi
  if (-not $process.Start()) { throw "failed to start $exePath" }
  return $process
}

function Invoke-ClientCaptured([string[]] $arguments) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName               = $clientExe
  $psi.Arguments              = ($arguments -join ' ')
  $psi.UseShellExecute        = $false
  $psi.CreateNoWindow         = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError  = $true
  $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
  $psi.StandardErrorEncoding  = [System.Text.Encoding]::UTF8
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $psi
  [void]$process.Start()
  $out = $process.StandardOutput.ReadToEnd()
  $err = $process.StandardError.ReadToEnd()
  $process.WaitForExit()
  return New-Object psobject -Property @{
    ExitCode = $process.ExitCode
    Stdout   = $out
    Stderr   = $err
  }
}

function Test-PortOpen([int] $targetPort) {
  $client = New-Object System.Net.Sockets.TcpClient
  try {
    $async = $client.BeginConnect('127.0.0.1', $targetPort, $null, $null)
    if (-not $async.AsyncWaitHandle.WaitOne(200)) { return $false }
    $client.EndConnect($async)
    return $true
  } catch {
    return $false
  } finally {
    $client.Close()
  }
}

# echo_client --bench prints:
#   qps        : 12345.6
#   latency_us : p50=1  p90=2  p95=3  p99=9  max=40  mean=2
#   errors     : 0 (0.00%)  overload=0  timeout=0
function Get-BenchMetrics([string] $text) {
  $result = New-Object psobject -Property @{
    Qps = $null; P50 = $null; P99 = $null; Max = $null; Errors = $null; Requests = $null
  }
  $match = [regex]::Match($text, '(?m)^qps\s*:\s*([0-9.]+)\s*$')
  if ($match.Success) { $result.Qps = [double]$match.Groups[1].Value }
  $match = [regex]::Match($text, '(?m)^latency_us\s*:\s*p50=(\d+)\s+p90=\d+\s+p95=\d+\s+p99=(\d+)\s+max=(\d+)')
  if ($match.Success) {
    $result.P50 = [int]$match.Groups[1].Value
    $result.P99 = [int]$match.Groups[2].Value
    $result.Max = [int]$match.Groups[3].Value
  }
  $match = [regex]::Match($text, '(?m)^errors\s*:\s*(\d+)\s*\(')
  if ($match.Success) { $result.Errors = [int]$match.Groups[1].Value }
  $match = [regex]::Match($text, '(?m)^requests\s*:\s*(\d+)')
  if ($match.Success) { $result.Requests = [int]$match.Groups[1].Value }
  return $result
}

function Get-Median([double[]] $values) {
  if ($values.Count -eq 0) { return $null }
  $sorted = @($values | Sort-Object)
  $middle = [int][math]::Floor($sorted.Count / 2)
  if ($sorted.Count % 2 -eq 1) { return $sorted[$middle] }
  return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
}

# ---------------------------------------------------------------------------
# 1. Start the server
# ---------------------------------------------------------------------------
Write-Step ("Starting echo_server on port {0} (metrics {1})" -f $Port, $MetricsPort)
$server = Start-ServerProcess $serverExe @(
  '--port', "$Port", '--metrics-port', "$MetricsPort", '--log-level', '4')

$ready = $false
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline) {
  if ($server.HasExited) { break }
  if (Test-PortOpen $Port) { $ready = $true; break }
  Start-Sleep -Milliseconds 100
}

$rows = New-Object System.Collections.ArrayList
$failed = 0

try {
  if (-not $ready) {
    if ($server.HasExited) {
      Stop-WithError ("server exited with code {0}" -f $server.ExitCode)
    }
    Stop-WithError 'server did not start'
  }
  Write-Host ("    listening on 127.0.0.1:{0} (pid {1})" -f $Port, $server.Id)

  # -------------------------------------------------------------------------
  # 2. Warmup (discarded)
  # -------------------------------------------------------------------------
  if ($Warmup -gt 0) {
    Write-Step ("Warmup: {0} requests" -f $Warmup)
    $warm = Invoke-ClientCaptured @('--servers', ("127.0.0.1:{0}" -f $Port),
                                    '--bench', "$Warmup", '1')
    if ($warm.ExitCode -ne 0) {
      Write-Host $warm.Stderr.TrimEnd()
      Stop-WithError ("warmup failed with exit code {0}" -f $warm.ExitCode)
    }
  }

  # -------------------------------------------------------------------------
  # 3. Sweep
  # -------------------------------------------------------------------------
  foreach ($threadCount in $threadList) {
    $qpsValues = @()
    $p50Values = @()
    $p99Values = @()
    $maxValues = @()
    $totalErrors = 0
    $totalRequests = 0

    for ($round = 1; $round -le $Rounds; $round++) {
      $run = Invoke-ClientCaptured @('--servers', ("127.0.0.1:{0}" -f $Port),
                                      '--bench', "$Requests", "$threadCount")
      if ($run.ExitCode -ne 0) {
        Write-Host $run.Stdout.TrimEnd()
        Write-Host $run.Stderr.TrimEnd()
        $failed++
        break
      }
      $metrics = Get-BenchMetrics $run.Stdout
      if ($null -eq $metrics.Qps) {
        Write-Host $run.Stdout.TrimEnd()
        Write-Host '    (could not parse the bench output)'
        $failed++
        break
      }
      Write-Host ("    threads={0} round={1}/{2}  qps={3:N1}  p50={4}us  p99={5}us  errors={6}" -f `
        $threadCount, $round, $Rounds, $metrics.Qps, $metrics.P50, $metrics.P99, $metrics.Errors)
      $qpsValues += $metrics.Qps
      if ($null -ne $metrics.P50) { $p50Values += [double]$metrics.P50 }
      if ($null -ne $metrics.P99) { $p99Values += [double]$metrics.P99 }
      if ($null -ne $metrics.Max) { $maxValues += [double]$metrics.Max }
      $totalErrors += $metrics.Errors
      $totalRequests += $metrics.Requests
    }

    if ($qpsValues.Count -gt 0) {
      [void]$rows.Add((New-Object psobject -Property @{
        Threads  = $threadCount
        QpsMed   = (Get-Median $qpsValues)
        QpsMin   = ($qpsValues | Measure-Object -Minimum).Minimum
        QpsMax   = ($qpsValues | Measure-Object -Maximum).Maximum
        P50      = (Get-Median $p50Values)
        P99      = (Get-Median $p99Values)
        Max      = (Get-Median $maxValues)
        Requests = $totalRequests
        Errors   = $totalErrors
      }))
    }
  }
} finally {
  if ($KeepServer) {
    Write-Step ("Server left running: pid {0} (stop with: Stop-Process -Id {0})" -f $server.Id)
  } elseif ($null -ne $server -and -not $server.HasExited) {
    Write-Step 'Stopping echo_server'
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    [void]$server.WaitForExit(5000)
  }
}

if ($rows.Count -eq 0) { Stop-WithError 'no benchmark round produced a result' }

# ---------------------------------------------------------------------------
# 4. Report
# ---------------------------------------------------------------------------

# Machine profile. A QPS number without these fields cannot be reproduced or
# explained, so they are collected with the number and printed next to it.
#
# Two levels of fallback on purpose:
#   * Get-CimInstance is the modern API but is missing on Windows 7 / PS 2 hosts;
#   * Get-WmiObject is deprecated but present almost everywhere;
#   * if both fail (locked-down host, WMI service disabled) we still print what the
#     environment gives us rather than an empty line -- a partial profile beats none,
#     as long as it is labelled as partial.
function Get-MachineProfile {
  # OS captions on a localized Windows are not ASCII ("Microsoft Windows 11 ..."),
  # and this script prints in the OEM code page, so a Chinese caption turns into
  # mojibake the moment it leaves the process. Strip to printable ASCII: a readable
  # partial label plus the build number is worth more than a garbled full one.
  function Convert-ToAscii([string] $text) {
    if ([string]::IsNullOrEmpty($text)) { return '' }
    $ascii = [regex]::Replace($text, '[^\x20-\x7E]', '')
    $ascii = [regex]::Replace($ascii, '\s{2,}', ' ').Trim()
    return $ascii
  }

  $profile = [ordered]@{
    CpuModel    = Convert-ToAscii $env:PROCESSOR_IDENTIFIER
    Cores       = 0
    Logical     = [Environment]::ProcessorCount
    MemoryGb    = 0.0
    OsCaption   = ''
    OsVersion   = [Environment]::OSVersion.VersionString
    Source      = 'environment only (WMI/CIM unavailable)'
  }

  try {
    $cpuInfo = Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop | Select-Object -First 1
    if ($null -ne $cpuInfo) {
      if ($cpuInfo.Name) { $profile.CpuModel = Convert-ToAscii $cpuInfo.Name }
      $profile.Cores = [int]$cpuInfo.NumberOfCores
      $profile.Source = 'CIM'
    }
  } catch {
    try {
      $cpuInfo = Get-WmiObject -Class Win32_Processor -ErrorAction Stop | Select-Object -First 1
      if ($null -ne $cpuInfo) {
        if ($cpuInfo.Name) { $profile.CpuModel = Convert-ToAscii $cpuInfo.Name }
        $profile.Cores = [int]$cpuInfo.NumberOfCores
        $profile.Source = 'WMI'
      }
    } catch { }
  }

  try {
    $osInfo = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
    if ($null -ne $osInfo) {
      $profile.OsCaption = Convert-ToAscii $osInfo.Caption
      $profile.OsVersion = ("{0} (build {1})" -f $osInfo.Version, $osInfo.BuildNumber)
      $profile.MemoryGb  = [math]::Round($osInfo.TotalVisibleMemorySize / 1MB, 1)
    }
  } catch {
    try {
      $osInfo = Get-WmiObject -Class Win32_OperatingSystem -ErrorAction Stop
      if ($null -ne $osInfo) {
        $profile.OsCaption = Convert-ToAscii $osInfo.Caption
        $profile.OsVersion = ("{0} (build {1})" -f $osInfo.Version, $osInfo.BuildNumber)
        $profile.MemoryGb  = [math]::Round($osInfo.TotalVisibleMemorySize / 1MB, 1)
      }
    } catch { }
  }

  if ([string]::IsNullOrWhiteSpace($profile.OsCaption)) {
    # Localized caption stripped down to nothing (e.g. a CJK-only edition name).
    $profile.OsCaption = 'Windows'
  }

  return $profile
}

$machine = Get-MachineProfile
$stamp = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')

# Spread guard. A median of 3 hides nothing when the machine is steadily noisy, and a
# steadily noisy machine is exactly what a shared build host is: measured here, two runs
# two minutes apart gave 19.2k and 4.7k QPS at 1 thread because other builds were running.
# So the spread is printed per row, and a point whose (max-min)/median exceeds 30% is
# flagged: those numbers are for "is it roughly this order of magnitude", not for a table.
$noisy = New-Object System.Collections.ArrayList
$lines = New-Object System.Collections.ArrayList
[void]$lines.Add('| threads | QPS (median) | QPS (min..max) | spread | p50 (us) | p99 (us) | max (us) | requests | errors |')
[void]$lines.Add('| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
foreach ($row in $rows) {
  $spread = 0.0
  if ($row.QpsMed -gt 0) { $spread = ($row.QpsMax - $row.QpsMin) / $row.QpsMed }
  $spreadText = ('{0:N0}%' -f ($spread * 100))
  if ($spread -gt 0.30) { [void]$noisy.Add($row.Threads) }
  [void]$lines.Add(("| {0} | {1:N0} | {2:N0}..{3:N0} | {4} | {5:N0} | {6:N0} | {7:N0} | {8} | {9} |" -f `
    $row.Threads, $row.QpsMed, $row.QpsMin, $row.QpsMax, $spreadText, $row.P50, $row.P99, `
    $row.Max, $row.Requests, $row.Errors))
}

Write-Host ''
Write-Host ($lines -join "`n")
if ($noisy.Count -gt 0) {
  Write-Host ''
  Write-Host ("    !! noisy: thread point(s) {0} spread more than 30% between rounds." -f `
    ($noisy -join ', ')) -ForegroundColor Yellow
  Write-Host  "       Do NOT quote those rows: repeat the run on an idle machine first."
}
Write-Host ''
Write-Host ("    cpu     : {0}" -f $machine.CpuModel)
Write-Host ("    cores   : {0} physical / {1} logical" -f $machine.Cores, $machine.Logical)
Write-Host ("    memory  : {0} GB" -f $machine.MemoryGb)
Write-Host ("    os      : {0} {1}" -f $machine.OsCaption, $machine.OsVersion)
Write-Host ("    source  : {0}" -f $machine.Source)
Write-Host ("    topology: echo_server and echo_client are two processes on THIS host,")
Write-Host  "              talking over loopback (127.0.0.1) - no network in between."
Write-Host ("    params  : requests/thread={0}, rounds={1} (median reported), warmup={2}" -f `
  $Requests, $Rounds, $Warmup)

$document = @()
$document += '# qzrpc benchmark'
$document += ''
$document += ("- time: {0}" -f $stamp)
$document += ("- cpu: {0}" -f $machine.CpuModel)
$document += ("- cores: {0} physical / {1} logical" -f $machine.Cores, $machine.Logical)
$document += ("- memory: {0} GB" -f $machine.MemoryGb)
$document += ("- os: {0} {1}" -f $machine.OsCaption, $machine.OsVersion)
$document += ("- profile source: {0}" -f $machine.Source)
$document += ("- params: requests/thread={0}, rounds={1} (median reported), warmup={2}" -f `
  $Requests, $Rounds, $Warmup)
$document += '- topology: client and server are two processes on the same host, over loopback.'
$document += '- connection: echo_client uses connections_per_endpoint=1, so all client'
$document += '  threads share ONE connection per server -> this table measures one-connection'
$document += '  round-trip throughput, NOT server capacity.'
$document += '- NOTE: loopback numbers are for RELATIVE comparison between revisions,'
$document += '  not an estimate of cross-network performance.'
if ($noisy.Count -gt 0) {
  $document += ''
  $document += ('- WARNING: thread point(s) {0} varied more than 30% between rounds;' -f ($noisy -join ', '))
  $document += '  the machine was busy. Re-run on an idle machine before quoting these rows.'
}
$document += ''
$document += $lines
$document += ''
[System.IO.File]::WriteAllLines($Output, $document)
Write-Host ("    written : {0}" -f $Output)

if ($failed -gt 0) {
  Write-Host ''
  Write-Host ("BENCH FAILED: {0} round(s) failed" -f $failed) -ForegroundColor Red
  exit 1
}
Write-Host ''
Write-Host ("BENCH OK: {0} thread point(s) x {1} round(s)" -f $rows.Count, $Rounds) -ForegroundColor Green
exit 0
