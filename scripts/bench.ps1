#Requires -Version 5.1
<#
  Qingzhou RPC - benchmark runner (docs/00-file-map.md B-08).

  Runs `echo_client --bench N T --conns C` over a matrix of thread counts x connections
  per endpoint, repeats each point several times, and prints a Markdown table that can be
  pasted straight into the README's performance section (the `<!-- BENCH -->` marker).

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

  READ THIS BEFORE QUOTING THE NUMBERS -- what a row means:
    Every row is labelled with BOTH knobs: `threads` and `conns` (connections per endpoint).
    A QPS table without the conns column is a WRONG table, and this was measured, not argued:
    same binary, same server, same moment --
      --conns 1  -> qps 24108  p50 230us  p99 1339us
      --conns 8  -> qps 43104  p50 157us  p99  277us   (1.79x)
    One connection serialises request/response, so with `-Conns 1` the sweep measures
    `one connection round-trip throughput`, NOT server capacity. With `-Conns 8+` it starts
    to measure the server. Both are legitimate; they answer different questions.

    `--conns` is always passed EXPLICITLY here, never left to echo_client's auto rule
    (`max(8,threads)` for --bench). Reason: an implicit default makes two releases
    incomparable without anyone noticing -- the old script recorded a table whose every
    row was silently 1 connection, and it looked exactly like a capacity table.

    The client prints where the value came from; this script parses that line and FAILS
    the round if it does not say `source=cli(--conns N)`. "The parameter looks like it was
    passed in" is not evidence -- `-Threads 1,4` arriving as the single value 14 is.

  WHY --max-retries 0 IS ALWAYS PASSED:
    A benchmark answers "how much can we serve", not "how much can we salvage". With the
    default retries (2), an `errors : 0` row can mean "zero requests failed" OR "every
    failed request succeeded on the second attempt" -- and the table cannot tell you which.
    The client prints its own counters for exactly this reason:
      [client] retry_total=N failover_total=N late_total=N
    This script requires that line, and FAILS the round when retry_total is not 0: with
    --max-retries 0 a nonzero retry_total means the flag did not take effect, i.e. the row
    is about to be labelled "0 errors" while it is not. `late_total` is recorded too --
    a late response can only exist after a timeout, so a nonzero value means the p99 in
    that row is a best case, not the latency.

  Also writes the machine profile into the output: a QPS number without the CPU model
  and core count is not reproducible, so it is not evidence. On a hybrid CPU (P-cores with
  SMT + E-cores without) the thread axis has TWO regimes, and a table that sweeps 1..32
  hides the seam instead of showing it -- see -Threads.

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts\bench.ps1
    powershell -ExecutionPolicy Bypass -File scripts\bench.ps1 -Threads 8,16,24 -Conns 1,2,4,8,16 -Requests 5000 -Rounds 5
    powershell -ExecutionPolicy Bypass -File scripts\bench.ps1 -Conns 1 -SkipBuild        (single-conn baseline)
    powershell -ExecutionPolicy Bypass -File scripts\bench.ps1 -SkipBuild -Output bench.md

  Exit code: 0 = every round ran and parsed, 1 = a round failed or its output did not parse.

  NOTE: keep this file ASCII-only (cmd.exe parses it as code page 936 in some setups).
#>
[CmdletBinding()]
param(
  [int]    $Port        = 16161,

  # 0 disables the built-in /metrics endpoint (the server then logs no metrics).
  [int]    $MetricsPort = 9101,

  # Comma-separated client thread counts, e.g. "8,16,24".
  #
  # Taken as a STRING and split by hand, for a measured reason: `-Threads 1,4` passed
  # through `powershell -File script.ps1 -Threads 1,4` arrives as ONE argument whose text
  # is "1,4", and casting that to [int[]] does not split it -- it produced a single
  # element 14 (the comma silently swallowed), so the sweep ran a bogus 14-thread point.
  # Parsing the string here makes the CLI behave the way it reads.
  #
  # Default 8,16,24 is chosen for THIS class of machine, not for aesthetics. A hybrid CPU
  # (this host: i9-14900HX = 8 P-cores with SMT = 16 threads, plus 16 E-cores without SMT
  # = 16 more) has two regimes separated by the P-core thread count:
  #   * threads <= 16 : every worker can live on a P-core; the curve is the framework's
  #                     scalability;
  #   * threads >  16 : new workers land on E-cores, roughly half the single-core
  #                     throughput and no SMT sibling.
  # So 24 is here as ONE "oversubscription cost" probe, not as a point on a curve.
  # Sweeping 1..32 produces an ARCHITECTURE cost curve and prints it as if it were a
  # FRAMEWORK scalability curve -- those are two different claims and must not share a table.
  [string] $Threads     = '8,16,24',

  # Comma-separated connections PER ENDPOINT, e.g. "1,2,4,8,16".
  #
  # This axis is the one that was missing for the whole first round of measurement, and
  # without it the previous table was mislabelled rather than merely imprecise.
  # Measured on this host (single endpoint, 2000 req/thread): 1 -> 24.1k QPS, 8 -> 43.1k
  # (+79%), p99 1339us -> 277us. The knee is around conns == thread count; past that it is
  # flat, so 1,2,4,8,16 is enough to show the shape.
  [string] $Conns       = '1,2,4,8,16',

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

# "1,2,4,8" -> @(1,2,4,8). Rejects empty items and out-of-range values early: a 0-thread
# point would make echo_client fall back to its own default and quietly measure the wrong
# thing, which is the exact failure class this script is built to avoid.
function ConvertTo-IntList([string] $text, [int] $maxValue, [string] $label) {
  $list = @()
  foreach ($part in ($text -split ',')) {
    $trimmed = $part.Trim()
    if ($trimmed -eq '') { continue }
    $value = 0
    if (-not [int]::TryParse($trimmed, [ref]$value) -or $value -le 0 -or $value -gt $maxValue) {
      Stop-WithError ("invalid {0} '{1}' (expect 1..{2} in a comma-separated list)" -f `
        $label, $trimmed, $maxValue)
    }
    $list += $value
  }
  if ($list.Count -eq 0) { Stop-WithError ("no {0} given (-{1})" -f $label, $label) }
  return $list
}

$threadList = ConvertTo-IntList $Threads 256 'threads'
# echo_client rejects --conns outside 1..1024 (exit code 2); catch it here so a typo does
# not turn into a half-finished sweep with a few good rows and one dead point.
$connsList  = ConvertTo-IntList $Conns 1024 'conns'

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
# Helpers (same shape as scripts\demo.ps1 -- see the comments there)
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

# -1 is this script's "the client does not report this counter" marker; print it as n/a
# rather than as a number, so a missing field can never be read as a healthy zero.
function Format-Counter([int] $value) {
  if ($value -lt 0) { return 'n/a' }
  return [string]$value
}

function Get-Median([double[]] $values) {  if ($values.Count -eq 0) { return $null }
  $sorted = @($values | Sort-Object)
  $middle = [int][math]::Floor($sorted.Count / 2)
  if ($sorted.Count % 2 -eq 1) { return $sorted[$middle] }
  return ($sorted[$middle - 1] + $sorted[$middle]) / 2.0
}

# echo_client announces its semantic counters right before exiting, in EVERY mode:
#   [client] retry_total=0 failover_total=0 late_total=0
# Without them "errors : 0" is ambiguous -- it can mean "nothing failed" or "everything
# that failed was retried successfully". The fields are captured separately because they
# answer different questions: retry = a resend after the peer may already have seen the
# request; failover = switched endpoint before sending a single byte; late = a response
# that arrived after the caller had given up (so a timeout happened).
#
# late_total is OPTIONAL in the pattern on purpose: the counter set has already grown once
# (late_total was added after retry/failover), and a parser that hard-requires the newest
# field silently turns an older binary into "cannot prove retries were off". What must be
# non-negotiable is retry_total -- that one decides whether the row may say `errors : 0`.
function Get-ClientSemanticCounters([string] $text) {
  $match = [regex]::Match($text,
    '(?m)^\[client\]\s+retry_total=(\d+)\s+failover_total=(\d+)(?:\s+late_total=(\d+))?')
  if (-not $match.Success) { return $null }
  $late = -1  # -1 = the client did not report it
  if ($match.Groups[3].Success) { $late = [int]$match.Groups[3].Value }
  return New-Object psobject -Property @{
    Retry    = [int]$match.Groups[1].Value
    Failover = [int]$match.Groups[2].Value
    Late     = $late
  }
}

# echo_client announces where every auto-derivable knob came from, e.g.
#   [client] connections_per_endpoint=8 source=cli(--conns 8) endpoints=1 total_conns=8
# `source=` may itself contain parentheses and spaces (`auto(bench threads=8 -> max(8,threads))`),
# so the capture is non-greedy up to the `endpoints=` marker rather than `\S+`.
function Get-ClientConnsSource([string] $text) {
  $match = [regex]::Match($text,
    'connections_per_endpoint=(\d+)\s+source=(.+?)\s+endpoints=(\d+)\s+total_conns=(\d+)')
  if (-not $match.Success) { return $null }
  return New-Object psobject -Property @{
    Conns     = [int]$match.Groups[1].Value
    Source    = $match.Groups[2].Value.Trim()
    Endpoints = [int]$match.Groups[3].Value
    Total     = [int]$match.Groups[4].Value
  }
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
  # Warmup uses the WIDEST conns point, so every server-side connection the sweep will
  # later use is already created and page-faulted in. It is discarded -- but its
  # announcement line is still checked: if `--conns` were being ignored, this is the
  # cheapest possible place to find out, before 15 points x N rounds have been recorded
  # under a label that is not true.
  $warmConns = [int]($connsList | Measure-Object -Maximum).Maximum
  if ($Warmup -gt 0) {
    Write-Step ("Warmup: {0} requests, threads=1, conns={1}" -f $Warmup, $warmConns)
    $warm = Invoke-ClientCaptured @('--servers', ("127.0.0.1:{0}" -f $Port),
                                    '--bench', "$Warmup", '1',
                                    '--conns', "$warmConns",
                                    '--max-retries', '0')
    if ($warm.ExitCode -ne 0) {
      Write-Host $warm.Stdout.TrimEnd()
      Write-Host $warm.Stderr.TrimEnd()
      Stop-WithError ("warmup failed with exit code {0}" -f $warm.ExitCode)
    }
    $warmSource = Get-ClientConnsSource $warm.Stdout
    if ($null -eq $warmSource) {
      Write-Host $warm.Stdout.TrimEnd()
      Stop-WithError 'warmup printed no [client] connections_per_endpoint line, so --conns cannot be verified'
    }
    if ($warmSource.Conns -ne $warmConns -or $warmSource.Source -notlike 'cli(*') {
      Stop-WithError ("warmup asked for conns={0} but the client reports conns={1} source={2} -- the flag did not take effect" -f `
        $warmConns, $warmSource.Conns, $warmSource.Source)
    }
  }

  # -------------------------------------------------------------------------
  # 3. Sweep: threads x conns
  # -------------------------------------------------------------------------
  foreach ($threadCount in $threadList) {
    foreach ($connCount in $connsList) {
      $qpsValues = @()
      $p50Values = @()
      $p99Values = @()
      $maxValues = @()
      $totalErrors = 0
      $totalRequests = 0
      $retryTotal = 0
      $failoverTotal = 0
      $lateTotal = 0
      $connSource = ''

      for ($round = 1; $round -le $Rounds; $round++) {
        $run = Invoke-ClientCaptured @('--servers', ("127.0.0.1:{0}" -f $Port),
                                        '--bench', "$Requests", "$threadCount",
                                        '--conns', "$connCount",
                                        '--max-retries', '0')
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
        # The row label has to be TRUE, not intended. The client states which source the
        # value came from; a row whose actual conns differ from the column it would be
        # printed under is a mislabelled measurement, and a mislabelled number is worse
        # than a missing one.
        $connsInfo = Get-ClientConnsSource $run.Stdout
        if ($null -eq $connsInfo) {
          Write-Host $run.Stdout.TrimEnd()
          Write-Host '    (no [client] connections_per_endpoint line -- cannot verify --conns)'
          $failed++
          break
        }
        if ($connsInfo.Conns -ne $connCount -or $connsInfo.Source -notlike 'cli(*') {
          Write-Host ("    asked conns={0}, client used conns={1} source={2}" -f `
            $connCount, $connsInfo.Conns, $connsInfo.Source)
          $failed++
          break
        }
        $connSource = $connsInfo.Source
        # "--max-retries 0" must be observable, not assumed: a nonzero retry_total means
        # the row's `errors : 0` is a post-retry number, and a table that cannot tell those
        # two apart is the same defect as a QPS row without its conns count.
        $counters = Get-ClientSemanticCounters $run.Stdout
        if ($null -eq $counters) {
          Write-Host $run.Stdout.TrimEnd()
          Write-Host '    (no [client] retry_total=... line -- cannot prove retries were off)'
          $failed++
          break
        }
        if ($counters.Retry -ne 0) {
          Write-Host ("    retry_total={0} with --max-retries 0 -- the flag did not take effect, or the request failed and was retried anyway" -f $counters.Retry)
          $failed++
          break
        }
        $retryTotal += $counters.Retry
        $failoverTotal += $counters.Failover
        # -1 means "this build does not report late_total". Once one round says that, the
        # row's late number is unknown for the whole point -- do not sum around it.
        if ($counters.Late -lt 0) { $lateTotal = -1 }
        elseif ($lateTotal -ge 0) { $lateTotal += $counters.Late }
        Write-Host ("    threads={0} conns={1} round={2}/{3}  qps={4:N1}  p50={5}us  p99={6}us  errors={7}  retry={8} failover={9} late={10}" -f `
          $threadCount, $connCount, $round, $Rounds, $metrics.Qps, $metrics.P50, $metrics.P99, $metrics.Errors,
          $counters.Retry, $counters.Failover, (Format-Counter $counters.Late))
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
          Conns    = $connCount
          Source   = $connSource
          QpsMed   = (Get-Median $qpsValues)
          QpsMin   = ($qpsValues | Measure-Object -Minimum).Minimum
          QpsMax   = ($qpsValues | Measure-Object -Maximum).Maximum
          P50      = (Get-Median $p50Values)
          P99      = (Get-Median $p99Values)
          Max      = (Get-Median $maxValues)
          Requests = $totalRequests
          Errors   = $totalErrors
          Retry    = $retryTotal
          Failover = $failoverTotal
          Late     = $lateTotal
        }))
      }
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
#
# The conns column is not decoration. Every row states the two knobs it was measured with,
# because a QPS number whose connection count is unknown is not a measurement -- the same
# binary measured 24.1k and 43.1k in the same minute purely by changing conns.
$noisy = New-Object System.Collections.ArrayList
$lines = New-Object System.Collections.ArrayList
[void]$lines.Add('| threads | conns | QPS (median) | QPS (min..max) | spread | p50 (us) | p99 (us) | max (us) | requests | errors | retry | failover | late |')
[void]$lines.Add('| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
foreach ($row in $rows) {
  $spread = 0.0
  if ($row.QpsMed -gt 0) { $spread = ($row.QpsMax - $row.QpsMin) / $row.QpsMed }
  $spreadText = ('{0:N0}%' -f ($spread * 100))
  if ($spread -gt 0.30) { [void]$noisy.Add(("threads={0}/conns={1}" -f $row.Threads, $row.Conns)) }
  [void]$lines.Add(("| {0} | {1} | {2:N0} | {3:N0}..{4:N0} | {5} | {6:N0} | {7:N0} | {8:N0} | {9} | {10} | {11} | {12} | {13} |" -f `
    $row.Threads, $row.Conns, $row.QpsMed, $row.QpsMin, $row.QpsMax, $spreadText, `
    $row.P50, $row.P99, $row.Max, $row.Requests, $row.Errors, `
    $row.Retry, $row.Failover, (Format-Counter $row.Late)))
}

Write-Host ''
# One Write-Host per line. Not for correctness -- the authoritative output is the Markdown
# file written below -- but because a multi-line string in a single Write-Host renders its
# last line glued to the next print in some capture setups, and a table that only LOOKS
# right in one terminal is the same class of problem this script exists to avoid.
foreach ($tableLine in $lines) { Write-Host $tableLine }
Write-Host ''
if ($noisy.Count -gt 0) {
  Write-Host ("    !! noisy: point(s) {0} spread more than 30% between rounds." -f `
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
Write-Host  "    shared  : both processes compete for the same cores, so these numbers are"
Write-Host  "              END-TO-END JOINT throughput, NOT a server capacity ceiling."
Write-Host  "              A real capacity number needs two machines."
Write-Host ("    axis    : threads={0} x conns/endpoint={1} (conns is ALWAYS explicit)" -f `
  ($threadList -join ','), ($connsList -join ','))
Write-Host ("    params  : requests/thread={0}, rounds={1} (median reported), warmup={2}" -f `
  $Requests, $Rounds, $Warmup)
if ($machine.Logical -gt $machine.Cores) {
  # A hybrid part shows up as logical > cores; which cores are P and which are E is a
  # model-specific fact the script cannot read, so it states the consequence instead of
  # guessing the split.
  Write-Host  "              hybrid CPU likely (logical > physical): past the P-core thread count,"
  Write-Host  "              new workers run on E-cores (approx. half the per-core throughput)."
  Write-Host  "              Treat threads > P-core-threads points as an oversubscription COST probe."
}

$document = @()
$document += '# qzrpc benchmark'
$document += ''
$document += ("- time: {0}" -f $stamp)
$document += ("- cpu: {0}" -f $machine.CpuModel)
$document += ("- cores: {0} physical / {1} logical" -f $machine.Cores, $machine.Logical)
$document += ("- memory: {0} GB" -f $machine.MemoryGb)
$document += ("- os: {0} {1}" -f $machine.OsCaption, $machine.OsVersion)
$document += ("- profile source: {0}" -f $machine.Source)
$document += ("- axis: threads={0} x conns/endpoint={1}" -f ($threadList -join ','), ($connsList -join ','))
$document += ("- params: requests/thread={0}, rounds={1} (median reported), warmup={2}" -f `
  $Requests, $Rounds, $Warmup)
$document += '- topology: client and server are two processes on the same host, over loopback.'
$document += '- shared-cpu: both processes compete for the same cores, so these numbers are'
$document += '  END-TO-END JOINT throughput, not a server capacity ceiling. A true capacity'
$document += '  number needs two machines.'
$document += '- retries: --max-retries 0 for every point, and retry_total must be 0 or the'
$document += '  round fails -- so `errors` is never a post-retry number.'
$document += '- connection: --conns is passed explicitly for every point and the client'
$document += '  announcement line is verified (`source=cli(--conns N)`); a row whose actual'
$document += '  conns differ from its label fails the run instead of being recorded.'
$document += '- reading: rows with conns=1 measure ONE connection round-trip throughput, not'
$document += '  server capacity. Rows with conns>=threads are where the server is the subject.'
$document += '- NOTE: loopback numbers are for RELATIVE comparison between revisions,'
$document += '  not an estimate of cross-network performance.'
if ($noisy.Count -gt 0) {
  $document += ''
  $document += ('- WARNING: point(s) {0} varied more than 30% between rounds;' -f ($noisy -join ', '))
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
Write-Host ("BENCH OK: {0} point(s) ({1} threads x {2} conns) x {3} round(s)" -f `
  $rows.Count, $threadList.Count, $connsList.Count, $Rounds) -ForegroundColor Green
exit 0
