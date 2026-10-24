#Requires -Version 5.1
<#
  Qingzhou RPC - smoke gate (CI). One conclusion line, one exit code.

  WHAT IT DOES (single instance by default)
    1. starts echo_server (port 16161, /metrics on 9101) as a child process
    2. waits until /metrics answers -- not until some stdout line appears, because the
       endpoint answering IS the thing being tested
    3. samples each server's request_total / error_total (baseline)
    4. runs echo_client --loop <Iterations> --max-retries 0 against it
       (retries OFF on purpose: a gate that passes because a failed call was re-sent is a
        gate that hides the failure. 0 also makes "fail=0" mean "no call ever failed".)
    5. samples the same counters again and asserts the DELTAS:
         sum(request_total delta) == Iterations   (every call arrived)
         sum(error_total delta)   == 0            (none of them failed)
         client exit code          == 0
    6. always stops every child it started (try/finally) and exits 0/1

  Deliberately NOT in here: colours, tables, step numbering, building, MSVC env import,
  registry. Those belong to scripts\demo.ps1 (for humans) and scripts\build.ps1 (for the
  toolchain). This file is only allowed to be fast and to answer "green or red".

  OUTPUT CONTRACT -- the only thing a caller should depend on
    Exactly one line on stdout, pure ASCII, space-separated key=value:
      SMOKE OK calls=50 ok=50 fail=0 instances=1 ms=138.5
      SMOKE OK calls=50 ok=50 fail=0 instances=2 split=25/25 ms=141.2
      SMOKE FAIL calls=50 ok=0 fail=50 instances=1 ms=1204.0 reason=call_failures

    keys    : SMOKE, OK|FAIL, calls, ok, fail, instances, [split], ms, [reason]
    calls   : number of client calls requested
    ok      : calls the server actually counted and did not flag as errors
    fail    : calls - ok (floored at 0)
    split   : per-instance request deltas "A/B"; present ONLY when -Instances 2
    ms      : wall clock for the whole gate, '.' decimal point regardless of locale
    reason  : one of missing_binary | port_busy | server_not_ready | client_exit |
              count_mismatch | metrics_unreadable

    exit code: 0 = OK, 1 = FAIL. Nothing else is written to stdout. Diagnostics go to
    stderr only when -Verbose is given, so a green run stays parseable by eye.

  USAGE
    powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1
    powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1 -BuildDir build_final
    powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1 -Iterations 200
    powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1 -Instances 2
    powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1 -Verbose

  NOTE: keep this file ASCII-only (cmd.exe / PowerShell 5.1 read .ps1 as the OEM code
  page, 936 on this host; a UTF-8 Chinese literal is a parse error at worst, mojibake at
  best). The child processes print UTF-8 Chinese and that is fine -- this script never
  pattern-matches their text, it reads numbers out of /metrics instead.
#>
[CmdletBinding()]
param(
  # Directory holding example\echo_server.exe and example\echo_client.exe. Defaults to
  # the same 'build' that scripts\build.ps1 and scripts\ci.ps1 use, so a caller can pass
  # nothing at all.
  [string] $BuildDir = 'build',

  # Client calls to drive. 50 keeps the gate under a second on a warm build.
  [int]    $Iterations = 50,

  # 1 = one server (default, cheapest). 2 = two servers + round-robin, which also makes
  # the split= field appear -- use it when you want the load balancer asserted in CI too.
  [ValidateSet(1, 2)]
  [int]    $Instances = 1,

  [int]    $BasePort        = 16161,
  [int]    $BaseMetricsPort = 9101,

  [int]    $StartTimeoutSec = 15
)

$ErrorActionPreference = 'Stop'

$invariant = [System.Globalization.CultureInfo]::InvariantCulture
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir   = Split-Path -Parent $scriptDir
$buildPath = Join-Path $rootDir $BuildDir
$serverExe = Join-Path $buildPath 'example\echo_server.exe'
$clientExe = Join-Path $buildPath 'example\echo_client.exe'

$script:children = New-Object System.Collections.ArrayList
$script:reason   = ''
$script:calls    = 0
$script:ok       = 0
$script:fail     = 0
$script:split    = ''

function Write-Diag([string] $message) {
  # Only with -Verbose, and on stderr -- never on stdout.
  # Why stderr: a caller does "powershell ... smoke.ps1 > result.txt" and must get
  # exactly one line in that file. Write-Host would not even reach result.txt (in
  # PowerShell 5.1 it writes to the host UI, not to the output stream), which is why the
  # conclusion line below uses Write-Output instead.
  if ($VerbosePreference -ne 'SilentlyContinue') { [Console]::Error.WriteLine("[smoke] " + $message) }
}

# Launch through ProcessStartInfo rather than Start-Process.
# Measured on PowerShell 5.1: Start-Process rebuilds the child environment in a
# case-INSENSITIVE dictionary, so a host with both HTTP_PROXY and http_proxy (corporate
# proxy, CI, npm) throws "an item with the same key has already been added" before the
# child exists. ProcessStartInfo inherits the OS environment block as-is. The pipes are
# drained with ReadToEndAsync so a full 4 KB buffer can never block the child.
function Start-Child([string] $exe, [string[]] $arguments) {
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName               = $exe
  $info.Arguments              = ($arguments -join ' ')
  $info.UseShellExecute        = $false
  $info.CreateNoWindow         = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError  = $true
  $info.WorkingDirectory       = $rootDir

  $process = [System.Diagnostics.Process]::Start($info)
  $outTask = $process.StandardOutput.ReadToEndAsync()
  $errTask = $process.StandardError.ReadToEndAsync()
  [void]$script:children.Add([pscustomobject]@{
    Process = $process; OutTask = $outTask; ErrTask = $errTask
  })
  return $process
}

function Get-ChildText($task) {
  if ($null -eq $task) { return '' }
  try { if ($task.Wait(500)) { return $task.Result } } catch { }
  return ''
}

# Can we bind this port right now? Bind, not connect: on this host connecting to a
# closed loopback port takes ~2 s before it is refused (measured), which would make a
# six-port preflight slower than the test itself.
function Test-PortFree([int] $port) {
  $listener = $null
  try {
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $port)
    $listener.Start()
    return $true
  } catch {
    return $false
  } finally {
    if ($null -ne $listener) { try { $listener.Stop() } catch { } }
  }
}

# Wait for a previous run's children to finish tearing down instead of failing on the
# first probe -- back-to-back CI runs must not depend on how fast a process exits.
function Wait-PortsFree([int[]] $ports, [int] $timeoutMs) {
  $deadline = (Get-Date).AddMilliseconds($timeoutMs)
  while ($true) {
    $busy = @($ports | Where-Object { -not (Test-PortFree $_) })
    if ($busy.Count -eq 0) { return @() }
    if ((Get-Date) -ge $deadline) { return $busy }
    Start-Sleep -Milliseconds 100
  }
}

# Bare TCP GET instead of Invoke-WebRequest: no IE engine, no proxy auto-detection, no
# hidden dependencies. It also proves the endpoint is really HTTP/1.0 + Connection:
# close (there is no chunked body to parse).
function Get-MetricsText([int] $port) {
  $client = $null
  $stream = $null
  try {
    $client = New-Object System.Net.Sockets.TcpClient
    $connect = $client.BeginConnect('127.0.0.1', $port, $null, $null)
    if (-not $connect.AsyncWaitHandle.WaitOne(500)) { return $null }
    $client.EndConnect($connect)

    $stream = $client.GetStream()
    $stream.ReadTimeout = 2000
    $request = "GET /metrics HTTP/1.0`r`nHost: 127.0.0.1:$port`r`n`r`n"
    $requestBytes = [System.Text.Encoding]::ASCII.GetBytes($request)
    $stream.Write($requestBytes, 0, $requestBytes.Length)
    $stream.Flush()

    $buffer = New-Object byte[] 16384
    $bytes = New-Object System.IO.MemoryStream
    while ($true) {
      $read = $stream.Read($buffer, 0, $buffer.Length)
      if ($read -le 0) { break }
      $bytes.Write($buffer, 0, $read)
    }
    $text = [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())

    $separator = $text.IndexOf("`r`n`r`n")
    if ($separator -lt 0) { return $null }
    if ($text.Substring(0, $separator) -notmatch '^HTTP/1\.[01] 200') { return $null }
    return $text.Substring($separator + 4)
  } catch {
    return $null
  } finally {
    if ($null -ne $stream) { try { $stream.Dispose() } catch { } }
    if ($null -ne $client) { try { $client.Close() } catch { } }
  }
}

function Wait-Metrics([int] $port, [int] $timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ($null -ne (Get-MetricsText $port)) { return $true }
    Start-Sleep -Milliseconds 50
  }
  return $false
}

# Exact-name lookup; labelled series (name{code="OK"}) are skipped so a per-label
# counter can never be mistaken for the total.
function Get-MetricValue([string] $text, [string] $name) {
  if ([string]::IsNullOrEmpty($text)) { return $null }
  foreach ($line in ($text -split "`n")) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -eq 0) { continue }
    if ($trimmed[0] -eq '#') { continue }
    if ($trimmed.IndexOf('{') -ge 0) { continue }
    $parts = $trimmed -split '\s+'
    if ($parts.Count -lt 2) { continue }
    if ($parts[0] -ne $name) { continue }
    $parsed = 0.0
    if ([double]::TryParse($parts[1], [System.Globalization.NumberStyles]::Float,
                           $invariant, [ref]$parsed)) { return $parsed }
  }
  return $null
}

function Stop-Children {
  for ($i = $script:children.Count - 1; $i -ge 0; $i--) {
    $entry = $script:children[$i]
    try {
      if (-not $entry.Process.HasExited) {
        Stop-Process -Id $entry.Process.Id -Force -ErrorAction SilentlyContinue
        [void]$entry.Process.WaitForExit(3000)
      }
    } catch { }
  }
}

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
try {
  # -------------------------------------------------------------------------
  # 1. Binaries
  # -------------------------------------------------------------------------
  foreach ($exe in @($serverExe, $clientExe)) {
    if (-not (Test-Path -LiteralPath $exe)) {
      $script:reason = 'missing_binary'
      Write-Diag ("not found: {0} (run scripts\build.ps1 -BuildDir {1})" -f $exe, $BuildDir)
      break
    }
  }

  # -------------------------------------------------------------------------
  # 2. Ports
  # -------------------------------------------------------------------------
  $ports = @()
  $metricsPorts = @()
  if ($script:reason -eq '') {
    for ($i = 0; $i -lt $Instances; $i++) {
      $ports        += ($BasePort + $i)
      $metricsPorts += ($BaseMetricsPort + $i)
    }
    $busy = @(Wait-PortsFree ($ports + $metricsPorts) 3000)
    if ($busy.Count -gt 0) {
      $script:reason = 'port_busy'
      Write-Diag ("port(s) still in use after 3s: {0}" -f ($busy -join ', '))
    }
  }

  # -------------------------------------------------------------------------
  # 3. Servers up
  # -------------------------------------------------------------------------
  if ($script:reason -eq '') {
    for ($i = 0; $i -lt $Instances; $i++) {
      [void](Start-Child $serverExe @(
        '--port', "$($ports[$i])", '--metrics-port', "$($metricsPorts[$i])", '--log-level', '3'))
      if (-not (Wait-Metrics $metricsPorts[$i] $StartTimeoutSec)) {
        $script:reason = 'server_not_ready'
        Write-Diag ("/metrics on {0} did not answer within {1}s" -f $metricsPorts[$i], $StartTimeoutSec)
        break
      }
      Write-Diag ("server {0} up on {1} (metrics {2})" -f $i, $ports[$i], $metricsPorts[$i])
    }
  }

  # -------------------------------------------------------------------------
  # 4. Baseline counters
  # -------------------------------------------------------------------------
  $baselineRequests = @()
  $baselineErrors   = @()
  if ($script:reason -eq '') {
    for ($i = 0; $i -lt $Instances; $i++) {
      $text = Get-MetricsText $metricsPorts[$i]
      $requests = Get-MetricValue $text 'qzrpc_server_request_total'
      $errors   = Get-MetricValue $text 'qzrpc_server_error_total'
      if (($null -eq $requests) -or ($null -eq $errors)) {
        $script:reason = 'metrics_unreadable'
        Write-Diag ("could not read request_total/error_total from port {0}" -f $metricsPorts[$i])
        break
      }
      $baselineRequests += $requests
      $baselineErrors   += $errors
    }
  }

  # -------------------------------------------------------------------------
  # 5. Drive traffic
  # -------------------------------------------------------------------------
  if ($script:reason -eq '') {
    $serverList = (($ports | ForEach-Object { "127.0.0.1:$_" }) -join ',')
    $script:calls = $Iterations
    Write-Diag ("echo_client --servers {0} --loop {1} --max-retries 0" -f $serverList, $Iterations)

    # echo_client logs to stderr; in PowerShell a native command's stderr becomes an
    # ErrorRecord, and with $ErrorActionPreference='Stop' that is a terminating error on
    # a perfectly healthy run. Exit code is the success signal, so stderr is downgraded
    # to data by capturing the two streams ourselves.
    $clientInfo = New-Object System.Diagnostics.ProcessStartInfo
    $clientInfo.FileName               = $clientExe
    $clientInfo.Arguments              = ('--servers {0} --loop {1} --max-retries 0' -f $serverList, $Iterations)
    $clientInfo.UseShellExecute        = $false
    $clientInfo.CreateNoWindow         = $true
    $clientInfo.RedirectStandardOutput = $true
    $clientInfo.RedirectStandardError  = $true
    $clientInfo.WorkingDirectory       = $rootDir
    $client = [System.Diagnostics.Process]::Start($clientInfo)
    $clientOut = $client.StandardOutput.ReadToEnd()
    $clientErr = $client.StandardError.ReadToEnd()
    $client.WaitForExit()
    $clientExit = $client.ExitCode

    if ($clientExit -ne 0) {
      $script:reason = 'client_exit'
      Write-Diag ("echo_client exited with {0}" -f $clientExit)
      foreach ($line in (@(($clientOut + "`n" + $clientErr) -split "`r?`n") | Where-Object { $_.Length -gt 0 } | Select-Object -Last 10)) {
        Write-Diag ("  " + $line)
      }
    }

    # -----------------------------------------------------------------------
    # 6. Assert the deltas
    # -----------------------------------------------------------------------
    if ($script:reason -eq '') {
      $deltaRequests = @()
      $deltaErrors = 0
      for ($i = 0; $i -lt $Instances; $i++) {
        $text = Get-MetricsText $metricsPorts[$i]
        $requests = Get-MetricValue $text 'qzrpc_server_request_total'
        $errors   = Get-MetricValue $text 'qzrpc_server_error_total'
        if (($null -eq $requests) -or ($null -eq $errors)) {
          $script:reason = 'metrics_unreadable'
          break
        }
        $deltaRequests += [double]($requests - $baselineRequests[$i])
        $deltaErrors   += [double]($errors - $baselineErrors[$i])
      }

      if ($script:reason -eq '') {
        $totalRequests = 0.0
        foreach ($value in $deltaRequests) { $totalRequests += $value }
        $script:ok   = [int][Math]::Max(0.0, $totalRequests - $deltaErrors)
        $script:fail = [int][Math]::Max(0, $Iterations - $script:ok)
        if ($Instances -ge 2) {
          $script:split = (($deltaRequests | ForEach-Object { [string][int]$_ }) -join '/')
        }

        # Every driven call must show up exactly once. Over-counting means something
        # else is talking to this port; under-counting means calls were lost before
        # reaching the dispatcher (both are failures, not "close enough").
        if ($totalRequests -ne [double]$Iterations) {
          $script:reason = 'count_mismatch'
          Write-Diag ("server counted {0} request(s), client sent {1}" -f $totalRequests, $Iterations)
        } elseif ($deltaErrors -ne 0.0) {
          $script:reason = 'call_failures'
          Write-Diag ("server reported {0} error response(s)" -f $deltaErrors)
        }
      }
    }
  }
} catch {
  if ($script:reason -eq '') { $script:reason = 'exception' }
  Write-Diag ("unhandled: {0}" -f $_.Exception.Message)
} finally {
  Stop-Children
}
$stopwatch.Stop()

$elapsedMs = $stopwatch.Elapsed.TotalMilliseconds.ToString('F1', $invariant)
$fields = @('SMOKE')
if ($script:reason -eq '') { $fields += 'OK' } else { $fields += 'FAIL' }
$fields += ('calls={0}' -f $script:calls)
$fields += ('ok={0}' -f $script:ok)
$fields += ('fail={0}' -f $script:fail)
$fields += ('instances={0}' -f $Instances)
if ($script:split -ne '') { $fields += ('split={0}' -f $script:split) }
$fields += ('ms={0}' -f $elapsedMs)
if ($script:reason -ne '') { $fields += ('reason={0}' -f $script:reason) }
Write-Output ($fields -join ' ')

if ($script:reason -eq '') { exit 0 }
exit 1
