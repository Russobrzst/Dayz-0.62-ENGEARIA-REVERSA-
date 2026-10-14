param([string]$VsDevCmd = (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat'))
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $VsDevCmd)) { throw "VS 2026 VsDevCmd not found: $VsDevCmd" }
$root = $PSScriptRoot
$bin = Join-Path (Split-Path $root -Parent) 'bin'
$null = New-Item -ItemType Directory -Force -Path $bin
$obj = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$sources = @(
  (Join-Path $root 'src\rotation_core.cpp'),
  (Join-Path $root 'src\bridge_win.cpp'),
  (Join-Path $root 'src\debug_binding_move.cpp'),
  (Join-Path $root 'src\move_win.cpp'),
  (Join-Path $root 'src\native_trace.cpp')
)
$include = '/I"{0}"' -f (Join-Path $root 'include')
$quotedSources = ($sources | ForEach-Object { '"{0}"' -f $_ }) -join ' '
$command = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cd /d "{2}" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /O2 /DDZ62_TRACE_ENABLED /DDZ62_EQUIPPED_ENABLED /DDZ62_RECTANGLE_FIXED /DDZ62_REEQUIP_ENABLED {1} /LD {3} /link bcrypt.lib /OUT:"{4}" && cl /nologo /std:c++20 /EHsc /W4 /WX /MT /O2 {1} "{5}" "{6}" /Fe:"{7}"' -f $VsDevCmd,$include,$obj,$quotedSources,(Join-Path $bin 'dayz_rotation_move_v12.dll'),(Join-Path $root 'src\debug_loader.cpp'),(Join-Path $root 'src\rotation_core.cpp'),(Join-Path $bin 'dayz_rotation_loader.exe')
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Native bridge build failed: $LASTEXITCODE" }
Get-FileHash (Join-Path $bin 'dayz_rotation_move_v12.dll') -Algorithm SHA256
Get-FileHash (Join-Path $bin 'dayz_rotation_loader.exe') -Algorithm SHA256
