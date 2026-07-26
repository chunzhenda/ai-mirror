param([string]$Action="build", [string]$Extra="")
$env:MSYSTEM = $null
$env:MSYS = $null
$env:MSYSTEM_PREFIX = $null
$env:MSYSTEM_CHOST = $null
$env:PYTHONIOENCODING = 'utf-8'
$env:IDF_PATH = 'e:\ESP-IDF\master\v5.1\esp-idf'
$env:IDF_TOOLS_PATH = 'e:\ESP-IDF\tools'
$pyExe = 'e:\ESP-IDF\tools\python_env\idf5.1_py3.11_env\Scripts\python.exe'

Write-Host "=== exporting IDF tools env ==="
$envOutput = & $pyExe "$env:IDF_PATH\tools\idf_tools.py" export --format key-value
if ($LASTEXITCODE -ne 0) { Write-Error "idf_tools.py export failed"; exit 1 }
foreach ($line in $envOutput) {
    if ($line -match '^([^#=]+)=(.*)$') {
        Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
    }
}

$env:PYTHON = $pyExe

# Add Git for Windows tools (patch.exe, git.exe) to PATH - needed by components
# that apply patches (e.g. esphome/micro-opus) and by ESP-IDF git checks.
foreach ($p in @("C:\Program Files\Git\usr\bin", "C:\Program Files\Git\mingw64\bin", "C:\Program Files\Git\cmd")) {
    if (Test-Path $p) { $env:PATH = "$p;" + $env:PATH }
}

Set-Location 'D:\UGit\ai-mirror\Firmware\esp32_ai_mirror'
Write-Host "=== running idf.py $Action $Extra ==="
switch ($Action) {
    "build"   { $env:CMAKE_BUILD_PARALLEL_LEVEL="2"; & $pyExe "$env:IDF_PATH\tools\idf.py" build }
    "flash"   { & $pyExe "$env:IDF_PATH\tools\idf.py" -p COM10 flash }
    "monitor" { & $pyExe "$env:IDF_PATH\tools\idf.py" -p COM10 monitor }
    "adddep"  { & $pyExe "$env:IDF_PATH\tools\idf.py" add-dependency $Extra }
    "reconf"  { & $pyExe "$env:IDF_PATH\tools\idf.py" reconfigure }
    "fullclean" { & $pyExe "$env:IDF_PATH\tools\idf.py" fullclean }
    default   { & $pyExe "$env:IDF_PATH\tools\idf.py" build }
}
exit $LASTEXITCODE
