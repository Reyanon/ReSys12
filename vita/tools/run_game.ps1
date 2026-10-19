# Run one game on the Vita for a while (measurements; the UDP logger records it), then ask the app to exit.
#   powershell -File vita/tools/run_game.ps1 -Game soulclbrac [-Seconds 90] [-Bench [-Slot N]] [-VitaIp 192.168.0.33]
# -Bench: start from the measurement scene ux0:data/resys12/state/bench/<set>.st (one-shot benchload file; the scenes
# are made by tools/bench_scenes.sh and uploaded with vita/tools/upload_bench.ps1). With -Slot N: from save slot N
# (state/<set>_<N>.st, scenes saved while playing)
# Uses the one-shot ux0:data/resys12/restart file (the app starts straight into that game). The installed eboot is not
# touched. If unreachable, report and do not retry.
param(
	[Parameter(Mandatory = $true)][string]$Game,
	[int]$Seconds = 90,
	[switch]$Bench,
	[int]$Slot = -1,
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

function Request-Exit {
	"1" | Out-File -Encoding ascii "$env:TEMP\resys12_exit.txt"
	curl.exe -s --max-time 5 -T "$env:TEMP\resys12_exit.txt" "ftp://${VitaIp}:1337/ux0:/data/resys12/exit" | Out-Null
	Start-Sleep -Seconds 3
	curl.exe -s --max-time 4 -Q "DELE ux0:/data/resys12/exit" "ftp://${VitaIp}:1337/ux0:/data/resys12/" 2>$null | Out-Null
	Send-VitaCmd "quit $TitleId"
	Start-Sleep -Seconds 2
}

Request-Exit
[IO.File]::WriteAllText("$env:TEMP\resys12_restart.txt", $Game)
curl.exe -s -S --max-time 10 -T "$env:TEMP\resys12_restart.txt" "ftp://${VitaIp}:1337/ux0:/data/resys12/restart"
if ($LASTEXITCODE -ne 0) { Write-Error "run_game: cannot write the restart file"; exit 1 }
if ($Bench) {
	[IO.File]::WriteAllText("$env:TEMP\resys12_bench.txt", "$Slot")
	curl.exe -s -S --max-time 10 -T "$env:TEMP\resys12_bench.txt" "ftp://${VitaIp}:1337/ux0:/data/resys12/benchload"
}
Send-VitaCmd "screen on"
Send-VitaCmd "launch $TitleId"
Write-Output "run_game: $Game for $Seconds s"
Start-Sleep -Seconds $Seconds
Request-Exit
Write-Output "run_game: $Game done"
