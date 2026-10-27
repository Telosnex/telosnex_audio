# Installs VB-CABLE (VB-Audio Virtual Cable, Driver Pack 45: x64 and ARM64)
# on a hosted Windows runner, which has no audio endpoints, and lets desktop
# apps use the microphone. VB-CABLE gives one render and one capture
# endpoint; what is played on "CABLE Input" is recorded from "CABLE Output",
# so the echo path is real. No reboot is needed (probe, 2026-10-27).
#
# VB-CABLE is donationware that may be copied as is; this downloads the
# unmodified pack from vb-audio.com and checks its hash. The pack's driver is
# signed by Microsoft (attestation), so no certificate needs trusting.
$ErrorActionPreference = 'Stop'
$sha256 = 'B950E39F01AF1D04EA623C8F6D8EB9B6EA5C477C637295FABF20631C85116BFB'
$zip = Join-Path $env:RUNNER_TEMP 'vbcable.zip'
$dir = Join-Path $env:RUNNER_TEMP 'vbcable'

for ($i = 1; ; $i++) {
  try {
    Invoke-WebRequest https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip -OutFile $zip
    break
  } catch {
    if ($i -ge 3) { throw }
    Start-Sleep 5
  }
}
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
if ($hash -ne $sha256) { throw "VBCABLE_Driver_Pack45.zip has sha256 $hash, expected $sha256" }
Expand-Archive $zip -DestinationPath $dir -Force

# -i -h: install without a window. The x64 setup installs the ARM64 driver
# on ARM64 Windows (x64 emulation).
$setup = Start-Process (Join-Path $dir 'VBCABLE_Setup_x64.exe') -ArgumentList '-i', '-h' -PassThru
if (-not $setup.WaitForExit(300000)) {
  $setup | Stop-Process -Force
  throw 'VBCABLE_Setup_x64.exe did not finish in 300 s'
}
if ($setup.ExitCode -ne 0) { throw "VBCABLE_Setup_x64.exe exited with $($setup.ExitCode)" }

# The machine-wide consent is Allow on the runners, but the user's is unset,
# and then WASAPI capture fails for desktop apps (probe: request failed -4).
$consent = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone'
foreach ($key in $consent, "$consent\NonPackaged") {
  New-Item $key -Force | Out-Null
  Set-ItemProperty $key -Name Value -Value Allow
}

for ($i = 0; $i -lt 30; $i++) {
  $endpoints = @(Get-PnpDevice -Class AudioEndpoint -Status OK -ErrorAction SilentlyContinue |
    Where-Object FriendlyName -like '*VB-Audio*')
  if ($endpoints.Count -ge 2) { break }
  Start-Sleep 2
}
$endpoints | Format-Table Status, FriendlyName -AutoSize
if ($endpoints.Count -lt 2) { throw 'VB-CABLE endpoints did not appear' }
