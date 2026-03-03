@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM =========================
REM Config (pode sobrescrever ao chamar)
REM Ex: set OUTDIR=artifacts_v2 && run_train.bat
REM =========================
if "%PYTHON_BIN%"=="" set "PYTHON_BIN=python"
if "%VENV_DIR%"=="" set "VENV_DIR=.venv"
if "%OUTDIR%"=="" set "OUTDIR=artifacts"
if "%SEED%"=="" set "SEED=42"
if "%TEST_SIZE%"=="" set "TEST_SIZE=0.2"
if "%HIDDEN_LAYERS%"=="" set "HIDDEN_LAYERS=64,32,16"
if "%MAX_ITER%"=="" set "MAX_ITER=2000"

REM =========================
REM Checks
REM =========================
if not exist "train_model.py" (
  echo ERRO: train_model.py nao encontrado na raiz do repo.
  exit /b 1
)
if not exist "requirements.txt" (
  echo ERRO: requirements.txt nao encontrado na raiz do repo.
  exit /b 1
)

REM =========================
REM Create venv
REM =========================
if not exist "%VENV_DIR%\Scripts\python.exe" (
  echo [INFO] Criando venv em %VENV_DIR%
  "%PYTHON_BIN%" -m venv "%VENV_DIR%"
  if errorlevel 1 (
    echo ERRO: falha ao criar venv. Verifique instalacao do Python.
    exit /b 1
  )
)

REM =========================
REM Activate venv
REM =========================
call "%VENV_DIR%\Scripts\activate.bat"
if errorlevel 1 (
  echo ERRO: falha ao ativar venv.
  exit /b 1
)

echo [INFO] Atualizando pip
python -m pip install --upgrade pip >nul
if errorlevel 1 (
  echo ERRO: falha ao atualizar pip.
  exit /b 1
)

echo [INFO] Instalando dependencias
pip install -r requirements.txt
if errorlevel 1 (
  echo ERRO: falha ao instalar dependencias.
  exit /b 1
)

REM =========================
REM Locate data dir
REM =========================
set "DATA_DIR="
if exist "Laboratorio\" (
  set "DATA_DIR=Laboratorio"
) else if exist "Classificacao\" (
  set "DATA_DIR=Classificacao"
) else (
  echo ERRO: Nao encontrei as pastas Laboratorio\ nem Classificacao\ .
  exit /b 1
)

REM =========================
REM Build CSV lists (Normal vs Falha)
REM =========================
set "NORMAL_FILES="
set "FAULT_FILES="

REM Preferido: *SemVazamento*.csv e *ComVazamento*.csv
for %%F in ("%DATA_DIR%\*SemVazamento*.csv") do (
  set "NORMAL_FILES=!NORMAL_FILES! "%%~fF""
)
for %%F in ("%DATA_DIR%\*ComVazamento*.csv") do (
  set "FAULT_FILES=!FAULT_FILES! "%%~fF""
)

REM Fallback: normal/fault/vazamento (mais generico)
if "!NORMAL_FILES!"=="" (
  for %%F in ("%DATA_DIR%\*normal*.csv" "%DATA_DIR%\*Normal*.csv") do (
    if exist "%%~fF" set "NORMAL_FILES=!NORMAL_FILES! "%%~fF""
  )
)
if "!FAULT_FILES!"=="" (
  for %%F in ("%DATA_DIR%\*fault*.csv" "%DATA_DIR%\*Fault*.csv" "%DATA_DIR%\*vazamento*.csv" "%DATA_DIR%\*Vazamento*.csv") do (
    if exist "%%~fF" set "FAULT_FILES=!FAULT_FILES! "%%~fF""
  )
)

if "!NORMAL_FILES!"=="" (
  echo ERRO: Nao encontrei CSVs de NORMAL em %DATA_DIR%\ .
  echo Procurei por: *SemVazamento*.csv e variantes (normal/Normal).
  exit /b 1
)
if "!FAULT_FILES!"=="" (
  echo ERRO: Nao encontrei CSVs de FALHA em %DATA_DIR%\ .
  echo Procurei por: *ComVazamento*.csv e variantes (fault/Fault/vazamento).
  exit /b 1
)

echo [INFO] Dataset dir: %DATA_DIR%
echo [INFO] NORMAL CSVs: !NORMAL_FILES!
echo [INFO] FALHA  CSVs: !FAULT_FILES!

REM =========================
REM Run training
REM =========================
if not exist "%OUTDIR%\" mkdir "%OUTDIR%"

echo [INFO] Rodando treino...
python train_model.py ^
  --normal !NORMAL_FILES! ^
  --fault  !FAULT_FILES! ^
  --outdir "%OUTDIR%" ^
  --seed %SEED% ^
  --test_size %TEST_SIZE% ^
  --hidden_layers "%HIDDEN_LAYERS%" ^
  --max_iter %MAX_ITER% ^
  --split_by_file

if errorlevel 1 (
  echo ERRO: treino falhou. Veja o log acima.
  exit /b 1
)

echo.
echo [OK] Treino finalizado. Artefatos em: %OUTDIR%\
dir "%OUTDIR%"

endlocal
