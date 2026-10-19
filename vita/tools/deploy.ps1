# Windows deploy (same job as tools/deploy.sh). Use when the Vita is unreachable from WSL NAT.
#   powershell -File vita/tools/deploy.ps1 [-Eboot build-vita/eboot.bin] [-VitaIp 192.168.0.33]
# If unreachable, report failure and do not retry.
param(
	[string]$Eboot = "$PSScriptRoot\..\..\build-vita\eboot.bin",
	[string]$VitaIp = $(if ($env:VITA_IP) { $env:VITA_IP } else { '192.168.0.33' })
)
$ErrorActionPreference = 'Stop'
$TitleId = 'RSYS00012'

function Send-VitaCmd([string]$cmd) {
	$t = New-Object Net.Sockets.TcpClient
	try {
		$t.Connect($VitaIp, 1338)
		$s = $t.GetStream()
		$b = [Text.Encoding]::ASCII.GetBytes("$cmd`n")
		$s.Write($b, 0, $b.Length)
		Start-Sleep -Milliseconds 300
	} finally { $t.Close() }
}

# first ask the app to exit cleanly (exit file; the app checks and deletes it every 2 s). Forced quit only if it is still running after that
"1" | Out-File -Encoding ascii "$env:TEMP\resys12_exit.txt"
curl.exe -s --max-time 5 -T "$env:TEMP\resys12_exit.txt" "ftp://${VitaIp}:1337/ux0:/data/resys12/exit" | Out-Null
Start-Sleep -Seconds 3 # the app checks every 2 s (do not poll the FTP listing: piled-up connections stalled vitacompanion FTP)
# if the app was not running the exit file remains; delete it so the next launch does not quit right away
curl.exe -s --max-time 4 -Q "DELE ux0:/data/resys12/exit" "ftp://${VitaIp}:1337/ux0:/data/resys12/" 2>$null | Out-Null
try { Send-VitaCmd "quit $TitleId" } catch {
	Write-Error "deploy: cannot connect to ${VitaIp}:1338 (check vitacompanion)"
	exit 1
}
Start-Sleep -Seconds 2
# right after the app exits the file may be locked and give 550, so retry the upload once
curl.exe -s -S -T $Eboot "ftp://${VitaIp}:1337/ux0:/app/$TitleId/"
if ($LASTEXITCODE -ne 0) {
	Start-Sleep -Seconds 2
	curl.exe -s -S -T $Eboot "ftp://${VitaIp}:1337/ux0:/app/$TitleId/"
	if ($LASTEXITCODE -ne 0) { Write-Error "deploy: upload failed (curl $LASTEXITCODE)"; exit 1 }
}
# a broken upload once left the app folder without eboot.bin (ReiREM): check the size on the device
$want = (Get-Item $Eboot).Length
$list = curl.exe -s --max-time 8 "ftp://${VitaIp}:1337/ux0:/app/$TitleId/"
$line = $list | Where-Object { $_ -match '\seboot\.bin\s*$' }
if (-not $line -or -not ($line -match "\s$want\s")) { Write-Error "deploy: eboot.bin on the device does not match ($want bytes)"; exit 1 }
# keep the screen on while testing : wake the display before launching
Send-VitaCmd "screen on"
Send-VitaCmd "launch $TitleId"
Write-Output "deploy: $Eboot -> ${VitaIp} launched"
