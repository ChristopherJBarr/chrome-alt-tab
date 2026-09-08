param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
try {
	$expected = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\ChromeAltTab'))
	$root = [IO.Path]::GetFullPath($PSScriptRoot)
	if ($root -ne $expected) { throw 'Run the uninstaller from the installed ChromeAltTab directory.' }
	$marker = Get-Content -LiteralPath (Join-Path $root 'installation.json') -Raw | ConvertFrom-Json
	if ($marker.product -ne 'chrome-alt-tab' -or $marker.installRoot -ne $root) { throw 'Installation ownership check failed.' }
	# Never recurse through a directory junction or symbolic link during removal.
	$entries = @((Get-Item -LiteralPath $root)) + @(Get-ChildItem -LiteralPath $root -Force -Recurse)
	if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) { throw 'Remove directory links from the installation before uninstalling.' }
	if (-not $Quiet) {
		Add-Type -AssemblyName System.Windows.Forms
		$answer = [Windows.Forms.MessageBox]::Show('Remove Chrome Alt+Tab, its startup entry and local files? Afterwards, remove the Chrome Alt+Tab extension from each Chrome profile.', 'Uninstall Chrome Alt+Tab', 'YesNo', 'Question')
		if ($answer -ne 'Yes') { return }
	}
	$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
	try {
		$hostPath = 'Software\Google\Chrome\NativeMessagingHosts\org.chrome_alt_tab.host'
		$key = $registry.OpenSubKey($hostPath)
		$registered = $null
		if ($key) { try { $registered = [string]$key.GetValue('') } finally { $key.Dispose() } }
		if ($registered -eq (Join-Path $root 'native-host.json')) { $registry.DeleteSubKey($hostPath, $false) }
		$run = $registry.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run', $true)
		if ($run) {
			try { if ($run.GetValue('ChromeAltTab') -eq ('"' + (Join-Path $root 'chrome-alt-tab-switcher.exe') + '" --broker')) { $run.DeleteValue('ChromeAltTab', $false) } } finally { $run.Dispose() }
		}
		$exe = Join-Path $root 'chrome-alt-tab-switcher.exe'
		# Ask the broker to notify extensions and release the hook before exit.
		$quit = Start-Process -FilePath $exe -ArgumentList '--quit' -WindowStyle Hidden -PassThru
		if (-not $quit.WaitForExit(5000)) { throw 'Timed out requesting shutdown.' }
		Start-Sleep -Milliseconds 500
		Get-Process chrome-alt-tab-switcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -ErrorAction Stop
		$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'Chrome Alt+Tab.lnk'
		if (Test-Path -LiteralPath $shortcutPath) {
			$shell = New-Object -ComObject WScript.Shell
			try { if ($shell.CreateShortcut($shortcutPath).TargetPath -eq $exe) { Remove-Item -LiteralPath $shortcutPath } } finally { [Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) | Out-Null }
		}
		# root was resolved, restricted to the exact product directory, and checked
		# for reparse points above. No paths from the manifest drive deletion.
		Remove-Item -LiteralPath $root -Recurse -Force
		$registry.DeleteSubKeyTree('Software\Microsoft\Windows\CurrentVersion\Uninstall\ChromeAltTab', $false)
	} finally { $registry.Dispose() }
	Write-Output 'Chrome Alt+Tab removed. Remove its extension in each Chrome profile to finish.'
	if (-not $Quiet) { [Windows.Forms.MessageBox]::Show('Chrome Alt+Tab has been removed. In each Chrome profile, open Extensions and remove Chrome Alt+Tab.', 'Uninstalled', 'OK', 'Information') | Out-Null }
} catch {
	if (-not $Quiet) { Add-Type -AssemblyName System.Windows.Forms; [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Uninstall failed', 'OK', 'Error') | Out-Null }
	throw
}
