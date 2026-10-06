#requires -Version 7.0
param(
    [Parameter(Mandatory = $true)] [string] $FilePath,
    [Parameter(Mandatory = $true)] [string] $LogPath,
    [string[]] $Arguments = @(),
    [string] $WorkingDirectory = (Get-Location).Path,
    [hashtable] $EnvironmentVariables = @{}
)

$ErrorActionPreference = 'Stop'
$launchInfo = [System.Diagnostics.ProcessStartInfo]::new()
$launchInfo.FileName = $FilePath
$launchInfo.WorkingDirectory = [System.IO.Path]::GetFullPath($WorkingDirectory)
$launchInfo.UseShellExecute = $false
$launchInfo.CreateNoWindow = $true
$launchInfo.RedirectStandardOutput = $true
$launchInfo.RedirectStandardError = $true
$launchInfo.Environment.Clear()
$seenVariableNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$inheritedVariables = [System.Environment]::GetEnvironmentVariables('Process')
foreach ($variableName in $inheritedVariables.Keys) {
    if ($seenVariableNames.Add([string] $variableName)) {
        $launchInfo.Environment[[string] $variableName] = [string] $inheritedVariables[$variableName]
    }
}
foreach ($variableName in $EnvironmentVariables.Keys) {
    $launchInfo.Environment[[string] $variableName] = [string] $EnvironmentVariables[$variableName]
}
foreach ($argument in $Arguments) {
    $launchInfo.ArgumentList.Add($argument)
}
$nativeProcess = [System.Diagnostics.Process]::new()
$nativeProcess.StartInfo = $launchInfo
$outputLog = [System.IO.FileStream]::new([System.IO.Path]::GetFullPath($LogPath), [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
$errorLog = [System.IO.FileStream]::new([System.IO.Path]::GetFullPath($LogPath + '.stderr'), [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
try {
    [void] $nativeProcess.Start()
    $outputPump = $nativeProcess.StandardOutput.BaseStream.CopyToAsync($outputLog)
    $errorPump = $nativeProcess.StandardError.BaseStream.CopyToAsync($errorLog)
    $nativeProcess.WaitForExit()
    [void] $outputPump.GetAwaiter().GetResult()
    [void] $errorPump.GetAwaiter().GetResult()
    $resultCode = $nativeProcess.ExitCode
} finally {
    $outputLog.Dispose()
    $errorLog.Dispose()
    $nativeProcess.Dispose()
}
Get-Content -LiteralPath $LogPath -Tail 60
Get-Content -LiteralPath ($LogPath + '.stderr') -Tail 40
exit $resultCode
