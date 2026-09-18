# Flash the latest C5VRX-buzzer build as soon as the ESP32-C5 ROM bootloader
# answers on the port, then start the serial logger.
#
# Why: on the XIAO ESP32-C5 esptool's USB-JTAG reset neither enters the
# bootloader nor restarts the application, and opening the port while the
# application runs wedges it. Working procedure:
#   1. stop every serial logger, run this script (it waits up to 20 min)
#   2. unplug the board, hold BOOT, plug it in, release BOOT after 2 s
#   3. wait for "flash OK" in the log file, press RESET
#
# usage: powershell -ExecutionPolicy Bypass -File tools\flash_when_ready.ps1 [-Port COM5] [-LogDir <dir>]
param(
    [string]$Port = "COM5",
    [string]$LogDir = (Join-Path $PSScriptRoot "..\.pio\logs"),
    [string]$Esptool = "$env:USERPROFILE\.platformio\penv\Scripts\esptool.exe",
    [string]$Python = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe",
    [int]$Baud = 460800
)
$B = Join-Path $PSScriptRoot "..\.pio\build\xiao_c5"
New-Item -ItemType Directory -Force $LogDir | Out-Null
$out  = Join-Path $LogDir "flash_when_ready.log"
$flog = Join-Path $LogDir "flash_when_ready_esptool.log"
$slog = Join-Path $LogDir "serial.log"
$cmd  = Join-Path $LogDir "serial_cmd.txt"
function Log($m) { Add-Content -Path $out -Value ((Get-Date).ToString("HH:mm:ss") + " " + $m) }

Log ("armed; trying write-flash on $Port every few seconds (build " + (Get-Item "$B\firmware.bin").LastWriteTime.ToString("HH:mm:ss") + ")")
$deadline = (Get-Date).AddMinutes(20)
$done = $false
while (-not $done -and (Get-Date) -lt $deadline) {
    if (-not ([System.IO.Ports.SerialPort]::GetPortNames() -contains $Port)) { Start-Sleep -Seconds 1; continue }
    cmd /c "`"$Esptool`" --chip esp32c5 --port $Port --baud $Baud --before no-reset --after no-reset --connect-attempts 1 write-flash -z --flash-mode dio --flash-freq 40m --flash-size 8MB 0x2000 `"$B\bootloader.bin`" 0x8000 `"$B\partitions.bin`" 0x10000 `"$B\firmware.bin`" > `"$flog`" 2>&1"
    $txt = Get-Content $flog -Raw
    if ($LASTEXITCODE -eq 0 -and $txt -match 'Hash of data verified') {
        $done = $true
        Log "flash OK"
        Get-Content $flog | Select-String -Pattern 'Wrote|Hash of data' | ForEach-Object { Log $_.Line }
    } elseif ($txt -match 'Wrote|Hash of data|Erasing|Compressed') {
        Log "PARTIAL flash attempt failed (do not reset, keep BOOT mode):"
        Get-Content $flog | Select-String -Pattern 'Wrote|Hash|error|fatal' -CaseSensitive:$false | ForEach-Object { Log $_.Line }
    } else {
        Start-Sleep -Seconds 2
    }
}
if (-not $done) { Log "timeout: no successful flash within 20 min"; exit 1 }
Start-Sleep -Seconds 1
Start-Process -FilePath $Python -ArgumentList "`"$PSScriptRoot\serial_log.py`"", $Port, "115200", "`"$slog`"", "`"$cmd`"" -WindowStyle Hidden
Log "logger started ($slog); press RESET on the board"
