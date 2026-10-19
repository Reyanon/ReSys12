# Measure games from their measurement scenes (state/bench/<set>.st, tools/bench_scenes.sh), a short run each.
# The first ~300 frames after the load compile the scene's code (cold recompiler): leave them out of the averages.
#   powershell -File vita/tools/bench_all.ps1 [-Seconds 25] [-Slot N] [-Games tektagt,ehrgeiz,...]
# -Slot N: each game's save slot N (scenes saved while playing) instead of its measurement scene
param(
	[int]$Seconds = 25,
	[int]$Slot = -1,
	[string[]]$Games = @("tektagt","tekken3","ehrgeiz","soulclbr","toukon3","fgtlayer","pacapp2","sws99","tenkomor","kaiunqz","ghlpanic","aquarush")
)
# (from bash a list arrives as one "a,b,c" string)
$Games = $Games | ForEach-Object { $_ -split "," } | Where-Object { $_ }
foreach ($g in $Games) {
	powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\run_game.ps1" -Game $g -Seconds $Seconds -Bench -Slot $Slot
}
Write-Output "bench done"
