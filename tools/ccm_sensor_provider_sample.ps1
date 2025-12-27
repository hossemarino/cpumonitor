param(
  [float]$TempC = 62.5,
  [double]$PowerW = 38.2,
  [float]$FanRpm = 1450
)

# Simple demo provider for CCM.
# Starts a local named-pipe server at: \\.\pipe\ccm_sensors
# CCM sends: GET\n
# This script replies with key=value lines.

$pipeName = 'ccm_sensors'

Write-Host "CCM sample provider running on \\.\pipe\$pipeName"
Write-Host "Replying with: tempC=$TempC powerW=$PowerW fanRpm=$FanRpm"
Write-Host "Press Ctrl+C to stop."

while ($true) {
  $pipe = New-Object System.IO.Pipes.NamedPipeServerStream(
    $pipeName,
    [System.IO.Pipes.PipeDirection]::InOut,
    1,
    [System.IO.Pipes.PipeTransmissionMode]::Byte,
    [System.IO.Pipes.PipeOptions]::Asynchronous
  )

  $pipe.WaitForConnection()

  try {
    $sr = New-Object System.IO.StreamReader($pipe, [System.Text.Encoding]::UTF8, $false, 1024, $true)
    $sw = New-Object System.IO.StreamWriter($pipe, [System.Text.Encoding]::UTF8, 1024, $true)
    $sw.NewLine = "`n"
    $sw.AutoFlush = $true

    $line = $sr.ReadLine()
    if ($line -and $line.Trim().ToUpperInvariant() -eq 'GET') {
      $sw.WriteLine("tempC=$TempC")
      $sw.WriteLine("powerW=$PowerW")
      $sw.WriteLine("fanRpm=$FanRpm")
    } else {
      $sw.WriteLine("error=unknown_request")
    }
  } catch {
    # ignore
  } finally {
    try { $pipe.Disconnect() } catch {}
    $pipe.Dispose()
  }
}
