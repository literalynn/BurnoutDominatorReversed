#requires -Version 7.0
param(
    [Parameter(Mandatory = $true)] [string] $BuildDirectory,
    [ValidatePattern('^[A-Za-z0-9_]+$')] [string] $Target = 'ps2EntryRunner',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')] [string] $Configuration = 'Release',
    [ValidateRange(1, 64)] [int] $Jobs = 8,
    [string] $LogPath = '',
    [string] $MSBuildPath = ''
)

$ErrorActionPreference = 'Stop'
$resolvedBuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$projectFiles = @(Get-ChildItem -LiteralPath $resolvedBuildDirectory -Recurse -File -Filter ($Target + '.vcxproj'))
if ($projectFiles.Count -ne 1) {
    throw ('Expected one Visual Studio project for ' + $Target + '; found ' + $projectFiles.Count + '. Configure CMake with a Visual Studio x64 generator first.')
}
if (-not $MSBuildPath) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswherePath)) {
        throw 'vswhere.exe was not found. Pass -MSBuildPath with the full path to MSBuild.exe.'
    }
    $MSBuildPath = & $vswherePath -latest -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild/**/Bin/MSBuild.exe' | Select-Object -First 1
    if (-not $MSBuildPath) {
        throw 'MSBuild.exe was not found in an installed Visual Studio instance.'
    }
}
if (-not $LogPath) {
    $LogPath = Join-Path $resolvedBuildDirectory ('build-' + $Target.Replace(';', '-') + '.log')
}
$nativeArguments = @(
    $projectFiles[0].FullName,
    ('/p:Configuration=' + $Configuration),
    '/p:Platform=x64',
    '/p:MultiProcessorCompilation=true',
    ('/m:' + $Jobs),
    '/nr:false',
    '/verbosity:minimal'
)
$compilerSuffixOptions = [string] $env:_CL_
if ($compilerSuffixOptions) {
    $compilerSuffixOptions += ' '
}
$compilerSuffixOptions += ('/MP' + $Jobs)
& (Join-Path $PSScriptRoot 'Invoke-CleanEnvironment.ps1') -FilePath $MSBuildPath -LogPath $LogPath -WorkingDirectory $resolvedBuildDirectory -Arguments $nativeArguments -EnvironmentVariables @{ '_CL_' = $compilerSuffixOptions }
exit $LASTEXITCODE
