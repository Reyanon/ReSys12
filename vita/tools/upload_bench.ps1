# Upload measurement scenes (save states made by tools/bench_scenes.sh) to ux0:data/resys12/state/bench/.
#   powershell -File vita/tools/upload_bench.ps1 -Dir <folder with <set>.st files> [-VitaIp 192.168.0.33]
# They stay on the Vita only (they hold game RAM: never in the repository or the VPK).
param(
	[Parameter(Mandatory = $true)][string]$Dir,
	[string]$VitaIp = $(if ($env:VITA_IP) { $env:VITA_IP } else { '192.168.0.33' })
)
$ErrorActionPreference = 'Stop'
foreach ($f in Get-ChildItem -Path $Dir -Filter *.st) {
	curl.exe -s -S --max-time 60 --ftp-create-dirs -T $f.FullName "ftp://${VitaIp}:1337/ux0:/data/resys12/state/bench/$($f.Name)"
	if ($LASTEXITCODE -ne 0) { Write-Error "upload_bench: $($f.Name) failed"; exit 1 }
	Write-Output "upload_bench: $($f.Name)"
}
