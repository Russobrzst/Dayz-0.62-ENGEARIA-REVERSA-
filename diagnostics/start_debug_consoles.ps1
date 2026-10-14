param(
    [Parameter(Mandatory=$true)][string]$RuntimeRoot,
    [Parameter(Mandatory=$true)][string]$ClientExe,
    [Parameter(Mandatory=$true)][string]$ServerExe,
    [string]$LoaderLogDir = $RuntimeRoot,
    [string]$ClientProfileDir,
    [string]$ServerProfileDir,
    [string]$PythonwExecutable
)
$ErrorActionPreference = 'Stop'
$consoleScript = Join-Path $PSScriptRoot 'debug_console.py'
if (!$PythonwExecutable) {
    $pythonExecutable = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($pythonExecutable) { $PythonwExecutable = Join-Path (Split-Path $pythonExecutable.Source) 'pythonw.exe' }
    if (!(Test-Path -LiteralPath $PythonwExecutable)) {
        $pythonwCommand = Get-Command pythonw.exe -ErrorAction SilentlyContinue
        if ($pythonwCommand) { $PythonwExecutable = $pythonwCommand.Source }
    }
    if (!(Test-Path -LiteralPath $PythonwExecutable)) {
        $codexPythonw = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe'
        if (Test-Path -LiteralPath $codexPythonw) { $PythonwExecutable = $codexPythonw }
    }
}
if (!(Test-Path -LiteralPath $PythonwExecutable)) { throw 'pythonw.exe não localizado; informe -PythonwExecutable' }
foreach ($role in @('server','client1')) {
    $expectedExe = if ($role -eq 'server') { $ServerExe } else { $ClientExe }
    $profileDir = if ($role -eq 'server') { $ServerProfileDir } else { $ClientProfileDir }
    if (!(Test-Path -LiteralPath $expectedExe)) { throw "Executável não encontrado: $expectedExe" }
    $argumentLine = '"{0}" --role {1} --runtime-root "{2}" --game-exe "{3}" --loader-log-dir "{4}"' -f $consoleScript,$role,$RuntimeRoot,$expectedExe,$LoaderLogDir
    if ($profileDir) { $argumentLine += ' --profile-dir "{0}"' -f $profileDir }
    Start-Process -FilePath $PythonwExecutable -ArgumentList $argumentLine -WorkingDirectory $RuntimeRoot -WindowStyle Normal
}
