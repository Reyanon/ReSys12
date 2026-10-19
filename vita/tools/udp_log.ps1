# Vita UDP log receiver (Windows PowerShell 5.1)
# For PCs where python.exe is blocked by the firewall. Vita packets only arrive with the firewall rule "ReiREM Vita UDP log"
# (UDP 18194, allowing only the remote Vita IP).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\udp_log.ps1 [-Port 18194] [-Out log.txt]
param(
	[int]$Port = 18194,
	[string]$Out = ""
)

$client = New-Object System.Net.Sockets.UdpClient($Port)
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
[Console]::Error.WriteLine("UDP $Port listening (Ctrl+C to quit)")

try {
	while ($true) {
		$bytes = $client.Receive([ref]$remote)
		$line = $utf8.GetString($bytes).TrimEnd("`r", "`n")
		[Console]::Out.WriteLine("$($remote.Address) $line")
		[Console]::Out.Flush()
		if ($Out -ne "") {
			[System.IO.File]::AppendAllText($Out, $line + "`n", $utf8)
		}
	}
} finally {
	$client.Close()
}
