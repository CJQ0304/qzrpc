#Requires -Version 5.1
<#
  Qingzhou RPC - three-minute demo (Windows / PowerShell 5.1+). For humans, not for CI.
  The CI gate is scripts\smoke.ps1; this file is the one you screen-share.

  WHAT IT DOES, IN ORDER (every step prints what to expect before it shows the result)
    1. preflight   : find the binaries, make sure the ports are free
    2. registry    : start registry_main on 16160 (/metrics 9103)
    3. two servers : echo_server A on 16161 (/metrics 9101), B on 16162 (/metrics 9102),
                     both registering themselves into the registry
    4. round robin : echo_client --loop N --max-retries 0 over both endpoints -> split ~50/50
                     (--max-retries 0 is part of the check: with retries left on, a failed
                     call could be re-sent to the other instance and the split proves nothing)
    5. bench A     : echo_client --bench 1000 4, connection count chosen by the client
    6. bench B     : the same calls with --conns 1, plus each instance's own counters, so the
                     "N requests over 1 connection" claim is measured rather than asserted
    7. registry    : register_total / heartbeat_total -- registered AND still alive
    8. failover    : kill B, then 20 calls with --max-retries 0 -> 20/20 served by A
    9. wrap up     : summary, how to open example\web\index.html
    ... and it always stops every child it started (try/finally), unless -KeepRunning.

  USAGE
    powershell -ExecutionPolicy Bypass -File scripts\demo.ps1
    powershell -ExecutionPolicy Bypass -File scripts\demo.ps1 -BuildDir build_final
    powershell -ExecutionPolicy Bypass -File scripts\demo.ps1 -Iterations 200 -BenchCalls 2000
    powershell -ExecutionPolicy Bypass -File scripts\demo.ps1 -KeepRunning
    powershell -ExecutionPolicy Bypass -File scripts\demo.ps1 -SkipRegistry

  EXIT CODE
    0 = every step produced the expected result
    1 = a child failed to start, the client reported a failure, or a check failed

  WHY THIS FILE IS PURE ASCII
    cmd.exe and Windows PowerShell 5.1 parse .ps1 using the OEM code page (936 here). A
    UTF-8 Chinese literal is mojibake at best and a parse error at worst. The child
    processes still print Chinese; step 0 switches the console to UTF-8 so that output
    renders correctly, and this script never pattern-matches their text -- assertions read
    ASCII numbers out of /metrics instead.

  WHY .NET Process AND NOT Start-Process
    Windows environment variables are case-insensitive, PowerShell's env: drive is not.
    When both HTTP_PROXY and http_proxy exist (corporate proxy, CI, npm config),
    Start-Process enumerates env: into a case-insensitive dictionary and throws
    "An item with the same key has already been added" before the child starts.
    [System.Diagnostics.Process]::Start goes straight to the OS environment block.
    Output is drained with ReadToEndAsync so a full pipe can never block a child.
#>
[CmdletBinding()]
param(
  [string] $BuildDir = 'build',

  [int] $RegistryPort        = 16160,
  [int] $RegistryMetricsPort = 9103,
  [int] $ServerPortA         = 16161,
  [int] $ServerPortB         = 16162,
  [int] $MetricsPortA        = 9101,
  [int] $MetricsPortB        = 9102,

  # Sequential calls in step 4 (round robin). N must be even for a clean 50/50.
  [int] $Iterations = 100,

  # Step 5: calls per thread and thread count. 1000 x 4 = 4000 calls total.
  [int] $BenchCalls   = 1000,
  [int] $BenchThreads = 4,

  # Heartbeat period is ttl/3 = 5 s, so a shorter wait would show heartbeat=0 and make a
  # healthy registry look dead. 6 s buys exactly one round.
  [int] $RegistrySettleSec = 6,

  [int] $StartupTimeoutSec = 25,

  # Leave the servers running at the end so example\web\index.html has something to
  # scrape. Off by default: a demo that leaves listeners behind breaks the next run.
  [switch] $KeepRunning,

  # Skip the registry (step 2) and the --registry flag on the servers. Useful while
  # registry_main is not built.
  [switch] $SkipRegistry
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# 0. Console encoding. The children print UTF-8; the console defaults to CP936.
#    Setting OutputEncoding also calls SetConsoleOutputCP(65001).
# ---------------------------------------------------------------------------
try {
  $utf8 = New-Object System.Text.UTF8Encoding($false)
  [Console]::OutputEncoding = $utf8
  $OutputEncoding = $utf8
} catch {
  # A host without a console has no code page; not fatal.
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir   = Split-Path -Parent $scriptDir
$buildPath = Join-Path $rootDir $BuildDir

$script:failed  = 0
$script:steps   = 0
$script:started = New-Object System.Collections.ArrayList
$script:childrenStopped = $false

# Pre-seeded so the summary at the bottom can always be printed, including on the
# exception path where the steps that would have filled them never ran.
$useRegistry = $false
$loopA = 0
$loopB = 0
$benchQps = ''
$benchP99 = ''
$benchConns = ''
$reuseQps = ''
$capacityP99 = 0
$reuseP99 = 0
$failoverOk = $false
$rows = @(
  [pscustomobject]@{ Name = 'A'; Conn = 0; Req = 0 }
  [pscustomobject]@{ Name = 'B'; Conn = 0; Req = 0 }
)

function Write-Step([string] $title) {
  $script:steps++
  Write-Host ''
  Write-Host ("=== Step {0}: {1}" -f $script:steps, $title) -ForegroundColor Cyan
  Write-Host ''
}

function Write-Note([string] $message) { Write-Host ("    " + $message) -ForegroundColor DarkGray }
function Write-Ok([string] $message)   { Write-Host ("    OK   " + $message) -ForegroundColor Green }
function Write-Bad([string] $message) {
  $script:failed++
  Write-Host ("    FAIL " + $message) -ForegroundColor Red
}

function Stop-WithError([string] $message) {
  Write-Host ''
  Write-Host ("ERROR: " + $message) -ForegroundColor Red
  Write-Host ''
  Write-Host 'DEMO FAILED' -ForegroundColor Red
  Stop-Children
  exit 1
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Resolve-Exe([string] $name) {
  $candidates = @(
    (Join-Path $buildPath ('example\' + $name + '.exe')),
    (Join-Path $buildPath ($name + '.exe')),
    (Join-Path $buildPath ('Release\' + $name + '.exe')),          # VS multi-config
    (Join-Path $buildPath ('example\Release\' + $name + '.exe'))
  )
  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
  }
  return $null
}

# Can we bind this port right now?
#
# Measured on this host: a CONNECT probe is not an option -- connecting to a closed
# loopback port takes ~2 s before it is refused, so probing six ports would cost 12 s of
# preflight. A bind probe answers in microseconds and, checking the netstat output, the
# TIME_WAIT entries after a demo are client-side (local port = ephemeral), which do not
# block a listening bind. The weaker signal (Windows allows a second bind, SO_REUSEADDR
# without SO_EXCLUSIVEADDRUSE) is fine here because the readiness wait below is the real
# gate and the request-count assertions catch a stray listener stealing traffic.
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

# A child from a previous run can still be tearing down. Give it a moment instead of
# failing on the first probe -- two of my own tool invocations overlapped once and the
# preflight turned that into a confusing hard error.
function Wait-PortsFree([int[]] $ports, [int] $timeoutMs) {
  $deadline = (Get-Date).AddMilliseconds($timeoutMs)
  while ($true) {
    $busy = @($ports | Where-Object { -not (Test-PortFree $_) })
    if ($busy.Count -eq 0) { return @() }
    if ((Get-Date) -ge $deadline) { return $busy }
    Start-Sleep -Milliseconds 150
  }
}

function Start-Child([string] $label, [string] $exe, [string[]] $arguments) {
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
  [void]$script:started.Add([pscustomobject]@{
    Label = $label; Process = $process; OutTask = $outTask; ErrTask = $errTask
  })
  Write-Host ("    {0,-14} pid={1}" -f $label, $process.Id)
  return $process
}

function Get-ChildText($task) {
  if ($null -eq $task) { return '' }
  try { if ($task.Wait(800)) { return $task.Result } } catch { }
  return ''
}

function Stop-Children {
  if ($script:started.Count -eq 0) { return }
  # Stop-WithError stops children before exiting, and the finally block runs on the way
  # out too; without this guard every child gets reported (and re-killed) twice.
  if ($script:childrenStopped) { return }
  $script:childrenStopped = $true
  if ($KeepRunning) {
    Write-Host ''
    Write-Host '    -KeepRunning: children left up on purpose.' -ForegroundColor Yellow
    foreach ($entry in $script:started) {
      if (-not $entry.Process.HasExited) {
        Write-Host ("      {0,-14} pid={1}   stop: Stop-Process -Id {1}" -f $entry.Label, $entry.Process.Id)
      }
    }
    return
  }
  Write-Host ''
  Write-Host '    stopping children' -ForegroundColor DarkGray
  for ($i = $script:started.Count - 1; $i -ge 0; $i--) {
    $entry = $script:started[$i]
    try {
      if (-not $entry.Process.HasExited) {
        Stop-Process -Id $entry.Process.Id -Force -ErrorAction SilentlyContinue
        [void]$entry.Process.WaitForExit(3000)
        Write-Host ("      {0,-14} stopped" -f $entry.Label)
      } else {
        Write-Host ("      {0,-14} had already exited (code {1})" -f $entry.Label, $entry.Process.ExitCode)
      }
    } catch {
      Write-Host ("      {0,-14} could not be stopped: {1}" -f $entry.Label, $_.Exception.Message) -ForegroundColor Yellow
    }
  }
}

# Fetch /metrics over a bare TCP socket.
# Why not Invoke-WebRequest: it pulls in the IE parsing engine and the proxy
# auto-detection machinery, and on this host it can trip the same env: duplicate-key
# problem as Start-Process. A raw GET has no hidden dependencies, and it also proves the
# endpoint really is HTTP/1.0 + Connection: close (there is no chunked body to parse).
function Get-MetricsText([int] $port) {
  $client = $null
  $stream = $null
  try {
    $client = New-Object System.Net.Sockets.TcpClient
    $connect = $client.BeginConnect('127.0.0.1', $port, $null, $null)
    # 500 ms: a refused loopback connect costs ~2 s on this host before it fails, and
    # with the readiness loop below that would be the dominant cost of the whole preflight.
    if (-not $connect.AsyncWaitHandle.WaitOne(500)) { return $null }
    $client.EndConnect($connect)

    $stream = $client.GetStream()
    $stream.ReadTimeout = 3000
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
    $text = [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())

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

function Wait-Metrics([int] $port, [string] $label, $process) {
  $deadline = (Get-Date).AddSeconds($StartupTimeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ($null -ne (Get-MetricsText $port)) { return $true }
    # A bind failure (or any startup error) makes the child exit; without this the script
    # would keep polling a dead port for the full timeout and hide the child's own
    # message -- which is the only text that says *why*.
    if (($null -ne $process) -and $process.HasExited) {
      Write-Bad ("{0}: exited with code {1} before serving /metrics on {2}" -f $label, $process.ExitCode, $port)
      return $false
    }
    Start-Sleep -Milliseconds 200
  }
  Write-Bad ("{0}: /metrics on port {1} never answered within {2}s" -f $label, $port, $StartupTimeoutSec)
  return $false
}

# Exact-name lookup, labelled series (name{...}) skipped so a per-label counter can never
# be mistaken for the total.
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
    if ([double]::TryParse($parts[1], [ref]$parsed)) { return $parsed }
  }
  return $null
}

function Format-Metric($value) {
  if ($null -eq $value) { return 'n/a' }
  return ('{0:N0}' -f $value)
}

# Pull one percentile out of the client's "latency_us : p50=.. p99=.." line.
function Get-Percentile([string] $text, [string] $key) {
  if ([string]::IsNullOrEmpty($text)) { return 0 }
  foreach ($line in ($text -split "`r?`n")) {
    if ($line -match "^\s*latency_us\s*:.*\b$key=(\d+)") { return [int]$Matches[1] }
  }
  return 0
}

# Run a child and wait for it. Returns ExitCode plus the decoded text.
#
# Decisions worth explaining:
#   1. No 2>&1 anywhere. A native command's stderr becomes an ErrorRecord in PowerShell,
#      and with $ErrorActionPreference='Stop' that is a terminating error on a perfectly
#      healthy run. The two streams are captured separately instead.
#   2. StandardOutputEncoding = UTF8 is required: without it the pipe is decoded with the
#      console code page and the client's Chinese comes out as mojibake.
#   3. The text is echoed with [Console]::Out.WriteLine, NOT Write-Host and NOT Write-
#      Output. Measured: (a) Write-Output inside a function joins the function's return
#      value, which would corrupt the object this function returns; (b) Write-Host does
#      not travel into `powershell -File demo.ps1 > log.txt`; (c) Console.Out is correct
#      on a console, through a PowerShell pipeline, and into a redirect.
function Invoke-Client([string[]] $arguments) {
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName               = $script:clientExe
  $info.Arguments              = ($arguments -join ' ')
  $info.UseShellExecute        = $false
  $info.CreateNoWindow         = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError  = $true
  $info.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
  $info.StandardErrorEncoding  = New-Object System.Text.UTF8Encoding($false)
  $info.WorkingDirectory       = $rootDir

  $process = [System.Diagnostics.Process]::Start($info)
  # Read both pipes before WaitForExit; a full pipe buffer would otherwise deadlock.
  $stdout = $process.StandardOutput.ReadToEnd()
  $stderr = $process.StandardError.ReadToEnd()
  $process.WaitForExit()

  foreach ($line in ($stdout -split "`r?`n")) {
    if ($line.Length -gt 0) { [Console]::Out.WriteLine("    " + $line) }
  }
  foreach ($line in ($stderr -split "`r?`n")) {
    if ($line.Length -gt 0) { [Console]::Out.WriteLine("    " + $line) }
  }

  return [pscustomobject]@{ ExitCode = $process.ExitCode; Text = $stdout }
}

# ---------------------------------------------------------------------------
# Step 1: preflight
# ---------------------------------------------------------------------------
Write-Step 'Preflight - find the binaries, make sure the ports are free'

$script:clientExe = Resolve-Exe 'echo_client'
$script:serverExe = Resolve-Exe 'echo_server'
$script:registryExe = Resolve-Exe 'registry_main'

$missing = @()
if ($null -eq $script:serverExe)   { $missing += 'echo_server' }
if ($null -eq $script:clientExe)   { $missing += 'echo_client' }
if (($null -eq $script:registryExe) -and (-not $SkipRegistry)) { $missing += 'registry_main' }
if ($missing.Count -gt 0) {
  Write-Host ("    missing: {0}" -f ($missing -join ', ')) -ForegroundColor Red
  Write-Host ("    looked in: {0}" -f $buildPath)
  Stop-WithError 'build first: powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -BuildDir <dir>'
}
Write-Host ("    echo_server    {0}" -f $script:serverExe)
Write-Host ("    echo_client    {0}" -f $script:clientExe)
if ($null -ne $script:registryExe) { Write-Host ("    registry_main  {0}" -f $script:registryExe) }

$portsToCheck = @($ServerPortA, $ServerPortB, $MetricsPortA, $MetricsPortB)
if ((-not $SkipRegistry) -and ($null -ne $script:registryExe)) {
  $portsToCheck += $RegistryPort
  $portsToCheck += $RegistryMetricsPort
}
$busy = @(Wait-PortsFree $portsToCheck 3000)
if ($busy.Count -gt 0) {
  Write-Host ("    port(s) still in use after 3s: {0}" -f ($busy -join ', ')) -ForegroundColor Red
  Write-Host '    another demo or an old child process is holding them:'
  Write-Host ('      Get-NetTCPConnection -LocalPort {0} | Select-Object OwningProcess' -f $busy[0])
  Stop-WithError 'free the ports, or pass other -ServerPortA/-MetricsPortA values'
}
Write-Note ("ports free: {0}" -f ($portsToCheck -join ', '))
Write-Ok 'preflight'

try {
  # -------------------------------------------------------------------------
  # Step 2: registry
  # -------------------------------------------------------------------------
  $useRegistry = (-not $SkipRegistry) -and ($null -ne $script:registryExe)
  if ($useRegistry) {
    Write-Step ("Registry - registry_main on {0} (/metrics {1})" -f $RegistryPort, $RegistryMetricsPort)
    Write-Note 'expect: "QZRPC_READY" and /metrics answering on the metrics port'
    $registryProcess = Start-Child 'registry' $script:registryExe @(
      '--conf', (Join-Path $rootDir 'example\conf\registry.conf'),
      '--port', "$RegistryPort", '--log-level', '2')
    if (-not (Wait-Metrics $RegistryMetricsPort 'registry' $registryProcess)) {
      Stop-WithError 'registry did not become ready'
    }
    Write-Ok ("registry up: http://127.0.0.1:{0}/metrics" -f $RegistryMetricsPort)
  } else {
    Write-Step 'Registry - skipped'
    Write-Note 'the client uses static endpoints, so the demo still works without it;'
    Write-Note 'you only lose the "instances are registered and heartbeating" screen.'
  }

  # -------------------------------------------------------------------------
  # Step 3: two echo_servers
  # -------------------------------------------------------------------------
  Write-Step ('Two servers - A on {0} (/metrics {1}), B on {2} (/metrics {3})' -f `
              $ServerPortA, $MetricsPortA, $ServerPortB, $MetricsPortB)
  Write-Note 'same service, two instances. Same binary, different port.'
  $serverConf = Join-Path $rootDir 'example\conf\echo_server.conf'

  $baseArguments = @('--conf', $serverConf)
  if ($useRegistry) { $baseArguments += @('--registry', ("127.0.0.1:{0}" -f $RegistryPort)) }

  $processA = Start-Child 'echo_server_A' $script:serverExe @(
    $baseArguments + @('--port', "$ServerPortA", '--metrics-port', "$MetricsPortA", '--log-level', '3'))
  if (-not (Wait-Metrics $MetricsPortA 'echo_server A' $processA)) {
    Stop-WithError 'echo_server A did not become ready'
  }
  Write-Ok ("A up: port {0}, /metrics {1}" -f $ServerPortA, $MetricsPortA)

  $processB = Start-Child 'echo_server_B' $script:serverExe @(
    $baseArguments + @('--port', "$ServerPortB", '--metrics-port', "$MetricsPortB", '--log-level', '3'))
  if (-not (Wait-Metrics $MetricsPortB 'echo_server B' $processB)) {
    Stop-WithError 'echo_server B did not become ready'
  }
  Write-Ok ("B up: port {0}, /metrics {1}" -f $ServerPortB, $MetricsPortB)

  Write-Note 'note: --port/--metrics-port win over the values in echo_server.conf'
  Write-Note '(the binary loads the config first, then applies the command line on top).'

  $serverList = ("127.0.0.1:{0},127.0.0.1:{1}" -f $ServerPortA, $ServerPortB)

  # -------------------------------------------------------------------------
  # Step 4: round robin
  # -------------------------------------------------------------------------
  Write-Step ("Round robin - echo_client --loop {0} --max-retries 0 over both endpoints" -f $Iterations)
  Write-Note ("expect: ~{0}/{0} split, one long-lived connection per endpoint" -f ($Iterations / 2))
  Write-Note '--max-retries 0 is spelled out here on purpose: the split is only evidence if a'
  Write-Note 'failed call cannot be quietly re-sent to the other instance. Same reason as step 8.'
  Write-Note 'note: the two --loop INFO lines on stderr are the Channel announcing'
  Write-Note '      conn_per_ep=1 + strategy=round_robin. That single pool slot per endpoint'
  Write-Note '      is what step 6 then shows carrying thousands of calls.'

  $beforeA = Get-MetricValue (Get-MetricsText $MetricsPortA) 'qzrpc_server_request_total'
  $beforeB = Get-MetricValue (Get-MetricsText $MetricsPortB) 'qzrpc_server_request_total'

  $loopRun = Invoke-Client @('--servers', $serverList, '--loop', "$Iterations", '--max-retries', '0')
  if ($loopRun.ExitCode -ne 0) { Write-Bad ("echo_client --loop exited with {0}" -f $loopRun.ExitCode) }

  $textA = Get-MetricsText $MetricsPortA
  $textB = Get-MetricsText $MetricsPortB
  $afterA = Get-MetricValue $textA 'qzrpc_server_request_total'
  $afterB = Get-MetricValue $textB 'qzrpc_server_request_total'

  $loopA = if (($null -ne $beforeA) -and ($null -ne $afterA)) { [int]($afterA - $beforeA) } else { 0 }
  $loopB = if (($null -ne $beforeB) -and ($null -ne $afterB)) { [int]($afterB - $beforeB) } else { 0 }
  if (($loopA -gt 0) -and ($loopB -gt 0) -and (($loopA + $loopB) -eq $Iterations)) {
    Write-Ok ("traffic landed on both instances: A={0} B={1} of {2} ({3:N1}% / {4:N1}%)" -f `
              $loopA, $loopB, $Iterations, (100.0 * $loopA / $Iterations), (100.0 * $loopB / $Iterations))
  } else {
    Write-Bad ("split wrong: A={0} B={1}, expected both > 0 and A+B={2}" -f $loopA, $loopB, $Iterations)
  }

  # -------------------------------------------------------------------------
  # Step 5: bench, capacity mode (connection count chosen by the client)
  # -------------------------------------------------------------------------
  $benchTotal = $BenchCalls * $BenchThreads
  Write-Step ('Bench A - capacity mode: {0} threads x {1} calls = {2} calls' -f `
              $BenchThreads, $BenchCalls, $benchTotal)
  Write-Note 'expect: 0 errors, p99 well under 1 ms. The absolute QPS depends on the box;'
  Write-Note '        what makes it comparable is the [client] line below, which says how'
  Write-Note '        many connections per endpoint this run used and where that number came from.'

  $benchRun = Invoke-Client @('--servers', $serverList, '--bench', "$BenchCalls", "$BenchThreads")
  $benchQps = ''
  $benchP99 = ''
  foreach ($line in ($benchRun.Text -split "`r?`n")) {
    if ($line -match '^\s*qps\s*:\s*([0-9.]+)') { $benchQps = $Matches[1] }
    if ($line -match '^\s*latency_us\s*:\s*(.+)$') { $benchP99 = $Matches[1].Trim() }
    if ($line -match '^\[client\] connections_per_endpoint=(\d+)') { $benchConns = $Matches[1] }
  }
  if ($benchRun.ExitCode -ne 0) { Write-Bad ("echo_client --bench exited with {0}" -f $benchRun.ExitCode) }
  Write-Note 'max= is the first call on each connection (TCP + handshake); p99 excludes it.'
  Write-Note 'the "also:" line only appears when a status code other than overload/timeout occurs.'
  Write-Note 'check the last [client] line: retry_total=0 means "errors: 0" above was not'
  Write-Note '  produced by retrying anything (a retried call would show up there).'

  # -------------------------------------------------------------------------
  # Step 6: star screen -- the same load, forced onto one connection per endpoint
  # -------------------------------------------------------------------------
  Write-Step ('Bench B - reuse mode: same {0} calls, but --conns 1' -f $benchTotal)
  Write-Note 'this is the number that was silently broken before: with conns hard-wired to 1'
  Write-Note 'every thread queued on one connection. Here it is asked for explicitly, so the'
  Write-Note 'counters below mean what they say.'
  Write-Note 'connections/requests are deltas measured around this step only;'
  Write-Note 'active = connections open right now (the client has already exited, so 0).'

  $textA = Get-MetricsText $MetricsPortA
  $textB = Get-MetricsText $MetricsPortB
  $connBeforeA = Get-MetricValue $textA 'qzrpc_server_connection_total'
  $connBeforeB = Get-MetricValue $textB 'qzrpc_server_connection_total'
  $reqBeforeA  = Get-MetricValue $textA 'qzrpc_server_request_total'
  $reqBeforeB  = Get-MetricValue $textB 'qzrpc_server_request_total'

  $reuseRun = Invoke-Client @('--servers', $serverList, '--bench', "$BenchCalls", "$BenchThreads", '--conns', '1')
  $reuseQps = ''
  foreach ($line in ($reuseRun.Text -split "`r?`n")) {
    if ($line -match '^\s*qps\s*:\s*([0-9.]+)') { $reuseQps = $Matches[1] }
  }
  if ($reuseRun.ExitCode -ne 0) { Write-Bad ("echo_client --bench --conns 1 exited with {0}" -f $reuseRun.ExitCode) }

  $textA = Get-MetricsText $MetricsPortA
  $textB = Get-MetricsText $MetricsPortB
  $rows = @(
    [pscustomobject]@{ Name = 'A'; Port = $ServerPortA
      Conn = ([int]((Get-MetricValue $textA 'qzrpc_server_connection_total') - $connBeforeA))
      Req  = ([int]((Get-MetricValue $textA 'qzrpc_server_request_total')  - $reqBeforeA))
      Err  = [int](Get-MetricValue $textA 'qzrpc_server_error_total')
      P99  = Get-MetricValue $textA 'qzrpc_server_request_latency_us_p99'
      Mean = Get-MetricValue $textA 'qzrpc_server_request_latency_us_mean_us'
      Active = Get-MetricValue $textA 'qzrpc_server_connection_active' }
    [pscustomobject]@{ Name = 'B'; Port = $ServerPortB
      Conn = ([int]((Get-MetricValue $textB 'qzrpc_server_connection_total') - $connBeforeB))
      Req  = ([int]((Get-MetricValue $textB 'qzrpc_server_request_total')  - $reqBeforeB))
      Err  = [int](Get-MetricValue $textB 'qzrpc_server_error_total')
      P99  = Get-MetricValue $textB 'qzrpc_server_request_latency_us_p99'
      Mean = Get-MetricValue $textB 'qzrpc_server_request_latency_us_mean_us'
      Active = Get-MetricValue $textB 'qzrpc_server_connection_active' }
  )

  Write-Host ''
  Write-Host ('    {0,-5} {1,-7} {2,13} {3,10} {4,8} {5,10} {6,10} {7,9}' -f `
              'inst', 'port', 'connections', 'requests', 'errors', 'p99_us', 'mean_us', 'active')
  foreach ($row in $rows) {
    Write-Host ('    {0,-5} {1,-7} {2,13} {3,10} {4,8} {5,10} {6,10} {7,9}' -f `
                $row.Name, $row.Port, $row.Conn, (Format-Metric $row.Req), $row.Err,
                (Format-Metric $row.P99), (Format-Metric $row.Mean), (Format-Metric $row.Active))
  }
  Write-Host ''

  $totalReq  = ($rows | Measure-Object -Property Req -Sum).Sum
  $totalConn = ($rows | Measure-Object -Property Conn -Sum).Sum
  if (($totalConn -gt 0) -and ($totalReq -gt 0)) {
    Write-Host ('    {0} requests over {1} connection(s) -> {2:N0} requests per connection' -f `
                $totalReq, $totalConn, ($totalReq / $totalConn)) -ForegroundColor Green
    Write-Note 'one connection per endpoint carried every one of those calls; without the'
    Write-Note 'pool each call would pay a TCP + handshake round trip before its first byte.'
  } else {
    Write-Bad 'connection/request counters did not move -- /metrics may be stale'
  }
  if (($rows[0].Req + $rows[1].Req) -ne $benchTotal) {
    Write-Bad ("sent {0} calls but the servers counted {1}" -f $benchTotal, ($rows[0].Req + $rows[1].Req))
  }
  if (($benchQps -ne '') -and ($reuseQps -ne '')) {
    Write-Note 'both runs print their own qps above -- single observations, no noise control'
    Write-Note '(the same config on this box can differ 2x run to run). Do not quote qps as'
    Write-Note 'capacity; the p99 ratio below is the number that follows from the design.'
  }

  # p99 is the honest comparison: it is a direct consequence of "8 threads queueing on
  # one connection", and unlike qps it does not move with whatever else is running on
  # the machine. This pair is also printed in the summary.
  $capacityP99 = Get-Percentile $benchRun.Text 'p99'
  $reuseP99    = Get-Percentile $reuseRun.Text 'p99'
  if (($capacityP99 -gt 0) -and ($reuseP99 -gt 0)) {
    Write-Host ('    p99: capacity mode {0}us  vs  reuse mode {1}us  -> {2:N1}x' -f `
                $capacityP99, $reuseP99, ([double]$reuseP99 / [double]$capacityP99)) -ForegroundColor Green
    Write-Note 'same 4000 calls; the only difference is how many connections they queue on.'
  }

  # -------------------------------------------------------------------------
  # Step 7: registry view (both instances still alive at this point)
  # -------------------------------------------------------------------------
  if ($useRegistry) {
    Write-Step ("Registry view - who is registered ({0})" -f ("http://127.0.0.1:{0}/metrics" -f $RegistryMetricsPort))
    Write-Note ("waiting {0}s for one heartbeat round (heartbeat period = ttl/3 = 5s)" -f $RegistrySettleSec)
    Start-Sleep -Seconds $RegistrySettleSec
    $registryText = Get-MetricsText $RegistryMetricsPort
    $registerTotal = Get-MetricValue $registryText 'qzrpc_registry_register_total'
    $heartbeatTotal = Get-MetricValue $registryText 'qzrpc_registry_heartbeat_total'
    Write-Host ('    qzrpc_registry_register_total  {0}' -f (Format-Metric $registerTotal))
    Write-Host ('    qzrpc_registry_heartbeat_total {0}' -f (Format-Metric $heartbeatTotal))
    if ($registerTotal -lt 2) {
      Write-Bad ("register_total={0}, expected >= 2 (each echo_server registers itself)" -f $registerTotal)
    } elseif ($RegistrySettleSec -lt 5) {
      # Waiting less than one heartbeat period makes heartbeat=0 the correct reading;
      # calling that a failure would train people to ignore the check.
      Write-Ok 'both instances registered (heartbeat check skipped: -RegistrySettleSec < 5)'
    } elseif ($heartbeatTotal -lt 1) {
      Write-Bad ('register_total={0} but no heartbeat after {1}s: the instances went stale' -f $registerTotal, $RegistrySettleSec)
    } else {
      Write-Ok ("{0} instances registered, and both are still heartbeating (not just written once)" -f $registerTotal)
    }
  }

  # -------------------------------------------------------------------------
  # Step 8: failover with the retry budget at zero -- the screen the demo hangs on
  #
  # It has to be self-evidencing: with max_retries=0 there is no business retry left, so
  # 20/20 can only come from the failover layer (a request that never left the client is
  # moved to another endpoint without spending the budget -- channel.cpp's
  # "failover vs retry" comment). The proof is echo_client's own
  # "[client] max_retries=0 source=cli(--max-retries 0)" line.
  # B is restarted afterwards so the page hint that follows still has two live endpoints.
  # -------------------------------------------------------------------------
  Write-Step 'Failover - kill instance B, then 20 calls with --max-retries 0'
  Write-Note 'expect: 20/20 succeed and every response comes from A.'
  Write-Note 'why this is a proof and not a claim: with --max-retries 0 there is no retry'
  Write-Note '        budget left, so a call that never left the client can only be'
  Write-Note '        re-pointed at another endpoint by the failover layer.'
  Write-Note 'two lines to look for, both printed by the client itself:'
  Write-Note '  [client] max_retries=0 source=cli(--max-retries 0)'
  Write-Note '  [client] retry_total=0 failover_total=N   (N >= 1: switches really happened)'

  $textA = Get-MetricsText $MetricsPortA
  $reqBeforeFailover = Get-MetricValue $textA 'qzrpc_server_request_total'

  $victim = $script:started | Where-Object { $_.Label -eq 'echo_server_B' } | Select-Object -First 1
  if ($null -ne $victim) {
    if (-not $victim.Process.HasExited) {
      Stop-Process -Id $victim.Process.Id -Force -ErrorAction SilentlyContinue
      [void]$victim.Process.WaitForExit(3000)
    }
    Write-Host ("    killed echo_server_B (pid {0}) on purpose" -f $victim.Process.Id)
    $script:started.Remove($victim) | Out-Null
  } else {
    Write-Bad 'echo_server_B was not started by this script; cannot demonstrate failover'
  }

  $failoverRun = Invoke-Client @('--servers', $serverList, '--loop', '20', '--max-retries', '0')
  if ($failoverRun.ExitCode -ne 0) {
    Write-Bad ("failover run exited with {0}: a failed call means the failover layer did not cover it" -f $failoverRun.ExitCode)
  }

  # The client counts both budgets itself. Reading them beats asserting them: retry_total is
  # the number of business retries the run consumed (must be 0 here) and failover_total is the
  # number of endpoint switches (must be >= 1, otherwise nothing was ever re-pointed).
  $retryTotal = $null
  $failoverTotal = $null
  foreach ($line in ($failoverRun.Text -split "`r?`n")) {
    if ($line -match '^\[client\] retry_total=(\d+) failover_total=(\d+)') {
      $retryTotal = [int]$Matches[1]
      $failoverTotal = [int]$Matches[2]
    }
  }

  $textA = Get-MetricsText $MetricsPortA
  $failoverServed = [int]((Get-MetricValue $textA 'qzrpc_server_request_total') - $reqBeforeFailover)
  if ($null -eq $retryTotal) {
    Write-Bad 'echo_client did not print "[client] retry_total=.. failover_total=.." (cannot verify the claim)'
  } elseif (($failoverRun.ExitCode -eq 0) -and ($failoverServed -eq 20) -and ($retryTotal -eq 0) -and ($failoverTotal -ge 1)) {
    $failoverOk = $true
    Write-Ok ("20/20 served by A with retry_total=0, failover_total={0} (switches, not retries)" -f $failoverTotal)
  } elseif ($failoverServed -ne 20) {
    Write-Bad ("A counted {0} of the 20 calls -- either B was not really dead or calls were lost" -f $failoverServed)
  } elseif (($retryTotal -ne 0) -or ($failoverTotal -lt 1)) {
    Write-Bad ("client counters do not support the claim: retry_total={0} failover_total={1}" -f $retryTotal, $failoverTotal)
  }

  if ($null -ne $victim) {
    Write-Note 'restarting B so the next step (and -KeepRunning) leaves a healthy pair'
    $restartedB = Start-Child 'echo_server_B' $script:serverExe @(
      $baseArguments + @('--port', "$ServerPortB", '--metrics-port', "$MetricsPortB", '--log-level', '3'))
    if (Wait-Metrics $MetricsPortB 'echo_server B (restarted)' $restartedB) {
      Write-Ok ("B back up on port {0} (counters start from zero again)" -f $ServerPortB)
    } else {
      Write-Bad 'restarted echo_server B did not become ready'
    }
  }

  # -------------------------------------------------------------------------
  # Step 9: web page
  # -------------------------------------------------------------------------
  Write-Step 'Web page - example\web\index.html'
  Write-Note 'it fetches these two endpoints from the browser:'
  Write-Host ('      http://127.0.0.1:{0}/metrics' -f $MetricsPortA)
  Write-Host ('      http://127.0.0.1:{0}/metrics' -f $MetricsPortB)
  Write-Note 'Chrome / Edge / Firefox: double-click the file. The built-in endpoint sends'
  Write-Note '  Access-Control-Allow-Origin: * so Origin: null from file:// is allowed.'
  Write-Note 'Safari blocks fetch() from file:// -> serve the folder instead:'
  Write-Host ('      python -m http.server -d example/web 8080   then open http://127.0.0.1:8080/')
  if (-not $KeepRunning) {
    Write-Note 'the servers are about to be stopped, so re-run with -KeepRunning to watch the'
    Write-Note 'page update live.'
  }
} catch {
  Write-Bad $_.Exception.Message
  foreach ($entry in $script:started) {
    foreach ($pair in @(@('stdout', $entry.OutTask), @('stderr', $entry.ErrTask))) {
      $text = Get-ChildText $pair[1]
      if ([string]::IsNullOrEmpty($text)) { continue }
      Write-Host ("    tail of {0} {1}:" -f $entry.Label, $pair[0]) -ForegroundColor Yellow
      foreach ($line in (@($text -split "`r?`n") | Where-Object { $_.Length -gt 0 } | Select-Object -Last 12)) {
        Write-Host ("      " + $line)
      }
    }
  }
} finally {
  # The registry logs one status line every 3 s; it is the most human-readable proof of
  # "2 instances registered", so print the last few lines before the children go away.
  $registryEntry = $script:started | Where-Object { $_.Label -eq 'registry' } | Select-Object -First 1
  if ($null -ne $registryEntry) {
    foreach ($pair in @(@('stdout', $registryEntry.OutTask), @('stderr', $registryEntry.ErrTask))) {
      $registryText = Get-ChildText $pair[1]
      if ([string]::IsNullOrEmpty($registryText)) { continue }
      Write-Host ''
      Write-Host ("    registry {0} (verbatim, last 3 lines):" -f $pair[0]) -ForegroundColor DarkGray
      foreach ($line in (@($registryText -split "`r?`n") | Where-Object { $_.Length -gt 0 } | Select-Object -Last 3)) {
        Write-Host ('      ' + $line)
      }
    }
  }
  Stop-Children
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '================ summary ================' -ForegroundColor Cyan
Write-Host ("  instances        : 2 echo_server (ports {0} / {1}), registry {2}" -f `
            $ServerPortA, $ServerPortB, $(if ($useRegistry) { "on $RegistryPort" } else { 'skipped' }))
Write-Host ("  round robin      : A={0}  B={1}  of {2} sequential calls" -f $loopA, $loopB, $Iterations)
Write-Host ("  bench A (capacity): {0} calls, conns/ep={1}, p99={2}us" -f $benchTotal,
            $(if ($benchConns -ne '') { $benchConns } else { '?' }), $capacityP99)
Write-Host ("  bench B (reuse)  : {0} calls, conns/ep=1, p99={1}us" -f $benchTotal, $reuseP99)
if (($capacityP99 -gt 0) -and ($reuseP99 -gt 0)) {
  Write-Host ("  queueing cost    : p99 {0:N1}x worse when 4 threads share one connection" -f `
              ([double]$reuseP99 / [double]$capacityP99))
}
Write-Host ("  connection reuse : A {0} conn / {1} reqs,  B {2} conn / {3} reqs" -f `
            $rows[0].Conn, $rows[0].Req, $rows[1].Conn, $rows[1].Req)
Write-Host ("  failover         : {0}" -f $(if ($failoverOk) {
  ("20/20 with max_retries=0 after killing B (retry_total=0, failover_total={0})" -f $failoverTotal)
} else {
  'NOT VERIFIED -- see the FAIL above'
}))
Write-Host '  (qps omitted on purpose: it is a single observation on a shared machine.)'
Write-Host ''
if ($script:failed -gt 0) {
  Write-Host ("DEMO FAILED: {0} check(s) failed" -f $script:failed) -ForegroundColor Red
  exit 1
}
Write-Host ("DEMO OK: {0} steps" -f $script:steps) -ForegroundColor Green
exit 0
