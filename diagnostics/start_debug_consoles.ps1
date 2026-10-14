param(
    [Parameter(Mandatory=$true)][string]$RuntimeRoot,
    [Parameter(Mandatory=$true)][string]$ClientExe,
    [Parameter(Mandatory=$true)][string]$ServerExe,
    [string]$LoaderLogDir = $RuntimeRoot
)
$ErrorActionPreference = 'Stop'
$consoleScript = Join-Path $PSScriptRoot 'debug_console.py'
$pythonExecutable = (Get-Command python).Source
$pythonWindowed = Join-Path (Split-Path $pythonExecutable) 'pythonw.exe'
if (!(Test-Path -LiteralPath $pythonWindowed)) { throw 'pythonw.exe não localizado' }
foreach ($role in @('server','client1')) {
    $expectedExe = if ($role -eq 'server') { $ServerExe } else { $ClientExe }
    if (!(Test-Path -LiteralPath $expectedExe)) { throw "Executável não encontrado: $expectedExe" }
    $argumentLine = '"{0}" --role {1} --runtime-root "{2}" --game-exe "{3}" --loader-log-dir "{4}"' -f $consoleScript,$role,$RuntimeRoot,$expectedExe,$LoaderLogDir
    Start-Process -FilePath $pythonWindowed -ArgumentList $argumentLine -WorkingDirectory $RuntimeRoot -WindowStyle Normal
}
