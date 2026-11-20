$ErrorActionPreference = 'Stop'
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $installer)) {
    throw 'Install Visual Studio 2022 or Build Tools with Desktop development with C++ and a Windows SDK.'
}
$installation = & $installer -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio 2022 x86 C++ build tools were not found.'
}
$vcvars = Join-Path $installation 'VC/Auxiliary/Build/vcvars32.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw 'The x86 Visual Studio build environment was not found.'
}

$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$chunks = [ordered]@{
    coop_session = 'COOP_SESSION_LUA'
    coop_lobby = 'COOP_LOBBY_LUA'
    coop_display_mode = 'COOP_DISPLAY_MODE_LUA'
    coop_hand_ready = 'COOP_HAND_READY_LUA'
    coop_match_results = 'COOP_MATCH_RESULTS_LUA'
    coop_exchange = 'COOP_EXCHANGE_LUA'
    coop_showdown_nodes = 'COOP_SHOWDOWN_NODES_LUA'
    coop_showdown = 'COOP_SHOWDOWN_LUA'
    coop_bustout_nodes = 'COOP_BUSTOUT_NODES_LUA'
    coop_bustout = 'COOP_BUSTOUT_LUA'
    coop_control = 'COOP_CONTROL_LUA'
    coop_card_ui = 'COOP_CARD_UI_LUA'
    coop_pacing = 'COOP_PACING_LUA'
    coop_diagnostics = 'COOP_DIAGNOSTICS_LUA'
}
$header = foreach ($name in $chunks.Keys) {
    'static const char ' + $chunks[$name] + '[] ='
    foreach ($line in Get-Content -LiteralPath (Join-Path $PSScriptRoot "src/$name.lua") -Encoding UTF8) {
        '"' + $line.Replace('\', '\\').Replace('"', '\"') + '\n"'
    }
    ';'
}
$header | Set-Content -LiteralPath (Join-Path $build 'coop_exchange_lua.h') -Encoding UTF8
$batch = Join-Path $build 'build.cmd'
@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cl /nologo /LD /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX src\dinput8_proxy.c /Fo:build\dinput8_proxy.obj /Fe:build\dinput8.dll /link /DEF:src\dinput8_proxy.def /MACHINE:X86 user32.lib kernel32.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding Ascii
Push-Location -LiteralPath $PSScriptRoot
try {
    & cmd.exe /d /c build\build.cmd
    if ($LASTEXITCODE -ne 0) { throw "x86 build failed with exit code $LASTEXITCODE." }
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $build 'dinput8.dll')
} finally {
    Pop-Location
}
