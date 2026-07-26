@echo off
set "IDF_PATH=E:\ESP-IDF\master\v5.1\esp-idf"
echo === exporting IDF tools env ===
python.exe "%IDF_PATH%\tools\idf_tools.py" export --format key-value > "%TEMP%\idf_env.tmp"
if errorlevel 1 (
    echo idf_tools.py export failed
    exit /b 1
)
for /f "usebackq tokens=1,2 eol=# delims==" %%a in ("%TEMP%\idf_env.tmp") do call set "%%a=%%b"
del "%TEMP%\idf_env.tmp"
echo === checking python deps ===
python.exe "%IDF_PATH%\tools\idf_tools.py" check-python-dependencies
cd /d D:\UGit\ai-mirror\Firmware\esp32_ai_mirror
echo === running idf.py build ===
python.exe "%IDF_PATH%\tools\idf.py" build
