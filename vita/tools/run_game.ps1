# Run one game on the Vita for a while (measurements; the UDP logger records it), then send the app back to its game list.
#   powershell -File vita/tools/run_game.ps1 -Game soulclbrac [-Seconds 90] [-Bench [-Slot N]] [-VitaIp 192.168.0.33]
# -Bench: start from the measurement scene ux0:data/resys12/state/bench/<set>.st (one-shot benchload file; the scenes
# are made by tools/bench_scenes.sh and uploaded with vita/tools/upload_bench.ps1). With -Slot N: from save slot N
# (state/<set>_<N>.st, scenes saved while playing)
# Switches games as ReST-V does: the one-shot restart file names the game, then the exit file with "n" makes the running
# app exit cleanly and relaunch itself into it (no home screen in between: the screen stays on, and a launch never meets
# an app still closing, error C1-2569-2). Only when the app is not running is it launched through vitacompanion.
# At the end "n" again without a restart file: the app goes back to its game list. The installed eboot is not touched.
# If unreachable, report and do not retry.
param(
	[Parameter(Mandatory = $true)][string]$Game,
	[int]$Seconds = 90,
	[switch]$Bench,
	[int]$Slot = -1,
	[string]$VitaIp = $(if ($env:VITA_IP) { $env:VITA_IP } else { '192.168.0.33' })
)
$ErrorActionPreference = 'Stop'
$TitleId = 'RSYS00012'
$Ftp = "ftp://${VitaIp}:1337/ux0:/data/resys12"

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

# exit file "n": returns $true when the running app took it (it checks every 2 s), else removes it and returns $false
function Request-Next {
	[IO.File]::WriteAllText("$env:TEMP\resys12_exit.txt", "n")
	# (uploaded as exit.tmp and renamed after the upload: the app never sees a half-written file)
	curl.exe -s --max-time 5 -T "$env:TEMP\resys12_exit.txt" "$Ftp/exit.tmp" -Q "-RNFR ux0:/data/resys12/exit.tmp" -Q "-RNTO ux0:/data/resys12/exit" | Out-Null
	for ($i = 0; $i -lt 6; $i++) {
		Start-Sleep -Seconds 1
		$ls = curl.exe -s --max-time 4 --list-only "$Ftp/"
		if ($LASTEXITCODE -eq 0 -and -not ($ls -split "`r?`n" -contains 'exit')) { return $true }
	}
	curl.exe -s --max-time 4 -Q "DELE ux0:/data/resys12/exit" "$Ftp/" 2>$null | Out-Null
	return $false
}

[IO.File]::WriteAllText("$env:TEMP\resys12_restart.txt", $Game)
curl.exe -s -S --max-time 10 -T "$env:TEMP\resys12_restart.txt" "$Ftp/restart"
if ($LASTEXITCODE -ne 0) { Write-Error "run_game: cannot write the restart file"; exit 1 }
if ($Bench) {
	[IO.File]::WriteAllText("$env:TEMP\resys12_bench.txt", "$Slot")
	curl.exe -s -S --max-time 10 -T "$env:TEMP\resys12_bench.txt" "$Ftp/benchload"
}
if (-not (Request-Next)) { # (the app was not running)
	Send-VitaCmd "screen on"
	Send-VitaCmd "launch $TitleId"
}
Write-Output "run_game: $Game for $Seconds s"
Start-Sleep -Seconds $Seconds
# wait for the relaunch to reach the list: a restart file written before that is taken by the relaunching app, and
# the next run's "n" then ends that game while it loads
if (Request-Next) { Start-Sleep -Seconds 5 }
Write-Output "run_game: $Game done"
