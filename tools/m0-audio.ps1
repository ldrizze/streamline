# M0: per-process audio loopback. Plays a quiet 440 Hz tone in a separate process,
# captures only that PID (expect ~-23 dB), then explorer.exe (expect ~-96 dB = silence).
# Pass -TargetPid to probe a real app instead.
param([int]$TargetPid = 0)
$b = 'C:\msys64\ucrt64\bin'; $env:PATH = "$b;$env:PATH"

function Probe($procId, $label) {
  $out = & "$b\gst-launch-1.0.exe" -m wasapi2src loopback=true loopback-mode=include-process-tree `
    loopback-target-pid=$procId num-buffers=150 '!' audioconvert '!' level interval=500000000 '!' fakesink 2>&1 | Out-String
  $rms = [regex]::Matches($out, 'rms=\(GValueArray\)< ([-\d\.e]+)') | ForEach-Object { [math]::Round([double]$_.Groups[1].Value, 1) }
  "$label (pid $procId): rms dB = $($rms -join ', ')"
}

if ($TargetPid) { Probe $TargetPid 'alvo'; return }

# Started via Start-Process it is a child of this shell, so never probe $PID as the "other" process.
$tone = Start-Process -FilePath "$b\gst-launch-1.0.exe" -PassThru -WindowStyle Hidden -ArgumentList `
  'audiotestsrc', 'freq=440', 'volume=0.1', 'num-buffers=600', '!', 'audioconvert', '!', 'wasapi2sink'
Start-Sleep -Milliseconds 800
Probe $tone.Id 'tom (deve ter som)'
Probe (Get-Process explorer | Select-Object -First 1).Id 'explorer (deve ser silencio)'
$tone.WaitForExit(15000) | Out-Null
