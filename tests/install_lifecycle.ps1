param([Parameter(Mandatory=$true)][string]$Package)
# Run only on a clean disposable Windows user account (CI). Refuse to touch an
# existing user installation or development registration.
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:LOCALAPPDATA 'Programs\ChromeAltTab'
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
try {
	$key = $registry.OpenSubKey('Software\Google\Chrome\NativeMessagingHosts\org.chrome_alt_tab.host')
	if ($key) { $key.Dispose(); throw 'Use a clean test account: a host is already registered.' }
} finally { $registry.Dispose() }
if (Test-Path -LiteralPath $root) { throw 'Use a clean test account: installation folder exists.' }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('chrome-alt-tab-test-' + [Guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $Package -DestinationPath $stage
try {
	& (Join-Path $stage 'install.ps1') -Quiet
	$exe = Join-Path $root 'chrome-alt-tab-switcher.exe'
	function Command([string]$Argument) {
		$p = Start-Process -FilePath $exe -ArgumentList $Argument -WindowStyle Hidden -PassThru
		if (-not $p.WaitForExit(5000) -or $p.ExitCode -ne 0) { throw "Command failed: $Argument" }
		Start-Sleep -Milliseconds 300
	}
	Start-Sleep -Seconds 1
	Command '--pause'
	$logPath = Join-Path $root 'chrome-alt-tab-switcher.log'
	if ((Get-Content -LiteralPath $logPath -Raw) -notmatch 'keyboard hook removed') { throw 'Pause did not remove hook.' }
	Command '--resume'
	Command '--quit'
	if (Get-Process chrome-alt-tab-switcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) { throw 'Quit left a running process.' }
	$reg = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
	try {
		$run = $reg.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
		try { if ($run.GetValue('ChromeAltTab') -ne ('"' + $exe + '" --broker')) { throw 'Incorrect startup command.' } } finally { $run.Dispose() }
	} finally { $reg.Dispose() }
	Start-Process -FilePath $exe -ArgumentList '--broker' -WindowStyle Hidden
	Start-Sleep -Seconds 17
	if (-not (Get-Process chrome-alt-tab-switcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })) { throw 'Sign-in broker exited without Chrome.' }
	# An upgrade must preserve the stable installation location.
	& (Join-Path $stage 'install.ps1') -Quiet
	& (Join-Path $root 'uninstall.ps1') -Quiet
	if (Test-Path -LiteralPath $root) { throw 'Uninstall left files.' }
	$reg = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
	try {
		foreach ($path in @('Software\Google\Chrome\NativeMessagingHosts\org.chrome_alt_tab.host', 'Software\Microsoft\Windows\CurrentVersion\Uninstall\ChromeAltTab')) {
			$key = $reg.OpenSubKey($path)
			if ($key) { $key.Dispose(); throw "Uninstall left registration: $path" }
		}
		$run = $reg.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
		try { if ($run.GetValue('ChromeAltTab')) { throw 'Uninstall left startup entry.' } } finally { $run.Dispose() }
	} finally { $reg.Dispose() }
	if (Test-Path -LiteralPath (Join-Path ([Environment]::GetFolderPath('Programs')) 'Chrome Alt+Tab.lnk')) { throw 'Uninstall left shortcut.' }
	'PASS: install, pause, resume, quit, sign-in launch, idle persistence, upgrade and uninstall'
} finally {
	if (Test-Path -LiteralPath (Join-Path $root 'installation.json')) { & (Join-Path $root 'uninstall.ps1') -Quiet }
	$resolved = [IO.Path]::GetFullPath($stage)
	$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
	if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe staging cleanup.' }
	Remove-Item -LiteralPath $resolved -Recurse -Force
}
