param([string] $Python = '', [switch] $Diagnostic)
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Python) {
    $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($pythonCommand -and $pythonCommand.Source -notlike '*\WindowsApps\*') {
        $Python = $pythonCommand.Source
    } else {
        $bundledPython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
        if (Test-Path -LiteralPath $bundledPython) { $Python = $bundledPython }
        else { throw 'Python 3.11+ requis. Fournir -Python avec le chemin de python.exe.' }
    }
}
$candidateExecutables = @(
    (Join-Path $projectRoot 'dist\windows-x64\burnout_dominator.exe'),
    (Join-Path $projectRoot 'build\ps2xRuntime\Release\burnout_dominator.exe')
)
$gameExecutable = $candidateExecutables | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $gameExecutable) { throw 'Exécutable absent. Compiler le jeu avec tools/project.py.' }
$launchArguments = @((Join-Path $projectRoot 'tools\project.py'), 'run', '--exe', $gameExecutable)
if ($Diagnostic) { $launchArguments += @('--smoke-seconds', '10') }
Push-Location $projectRoot
try {
    & $Python @launchArguments
    $resultCode = $LASTEXITCODE
} finally { Pop-Location }
exit $resultCode
