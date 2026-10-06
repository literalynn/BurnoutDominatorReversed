[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GhidraInstall,
    [string]$ProjectDirectory,
    [string]$ProjectName = 'BurnoutDominatorPAL',
    [string]$OutputDirectory,
    [string]$JavaExecutable = 'java.exe',
    [ValidateRange(10, 7200)][int]$TotalBudgetSeconds = 900,
    [switch]$RetryTimeouts
)

$ErrorActionPreference = 'Stop'
$ps2Repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $ProjectDirectory) { $ProjectDirectory = Join-Path $ps2Repository 'local/analysis/ghidra/project' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $ps2Repository 'local/decompiled' }
$ps2Ghidra = [IO.Path]::GetFullPath($GhidraInstall)
$ps2Project = [IO.Path]::GetFullPath($ProjectDirectory)
$ps2Output = [IO.Path]::GetFullPath($OutputDirectory)
$ps2State = Join-Path $ps2Repository 'local/analysis/ghidra/export-state'
$ps2UtilityJar = Join-Path $ps2Ghidra 'Ghidra/Framework/Utility/lib/Utility.jar'
if (-not (Test-Path -LiteralPath $ps2UtilityJar)) { throw 'Ghidra Utility.jar is missing.' }
if (-not (Test-Path -LiteralPath (Join-Path $ps2Project ($ProjectName + '.gpr')))) { throw 'The saved Ghidra project is missing.' }
New-Item -ItemType Directory -Path $ps2Output,(Join-Path $ps2State 'settings'),(Join-Path $ps2State 'cache'),(Join-Path $ps2State 'temp') -Force | Out-Null
$ps2Arguments = @(
    '-Xmx4G', '-XX:ParallelGCThreads=2', '-XX:CICompilerCount=2',
    '-Djava.system.class.loader=ghidra.GhidraClassLoader', '-Dfile.encoding=UTF8',
    '-Djava.awt.headless=true', '--enable-native-access=ALL-UNNAMED',
    "-Duser.home=$ps2State", "-Dapplication.settingsdir=$ps2State/settings",
    "-Dapplication.cachedir=$ps2State/cache", "-Dapplication.tempdir=$ps2State/temp", "-Djava.io.tmpdir=$ps2State/temp",
    '-cp', $ps2UtilityJar, 'ghidra.Ghidra', 'ghidra.app.util.headless.AnalyzeHeadless',
    $ps2Project, $ProjectName, '-process', 'SLES_546.27', '-noanalysis', '-readOnly',
    '-scriptPath', $PSScriptRoot, '-postScript'
)
$ps2LogPrefix = ''
if ($RetryTimeouts) {
    if (-not (Test-Path -LiteralPath (Join-Path $ps2Output 'manifest.json'))) { throw 'There is no initial export manifest to retry.' }
    $ps2LogPrefix = 'retry-'
    $ps2Arguments += @('RetryBurnoutPseudocode.java', $ps2Output)
} else {
    $ps2Arguments += @('ExportBurnoutPseudocode.java', $ps2Output, $TotalBudgetSeconds.ToString())
}
$ps2Arguments += @('-max-cpu', '4', '-log', (Join-Path $ps2Output ($ps2LogPrefix + 'ghidra.log')), '-scriptlog', (Join-Path $ps2Output ($ps2LogPrefix + 'scripts.log')))
$ps2Arguments | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $ps2Output ($ps2LogPrefix + 'launch-arguments.json')) -Encoding utf8
& $JavaExecutable @ps2Arguments *> (Join-Path $ps2Output ($ps2LogPrefix + 'console.log'))
$ps2ExitCode = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $ps2Output ($ps2LogPrefix + 'console.log')) -Tail 15
if ($ps2ExitCode -ne 0) { exit $ps2ExitCode }
$ps2ManifestFile = Join-Path $ps2Output 'manifest.json'
if (-not (Test-Path -LiteralPath $ps2ManifestFile)) { throw 'Ghidra produced no manifest; inspect console.log.' }
$ps2Manifest = Get-Content -Raw -LiteralPath $ps2ManifestFile | ConvertFrom-Json
$ps2Manifest.status_counts | ConvertTo-Json
if (-not $ps2Manifest.all_pseudocode_exported) { exit 2 }
