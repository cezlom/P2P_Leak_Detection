#!/usr/bin/env bash
set -euo pipefail

# ===== Config =====
PYTHON_BIN="${PYTHON_BIN:-python3}"
VENV_DIR="${VENV_DIR:-.venv}"
OUTDIR="${OUTDIR:-artifacts}"
SEED="${SEED:-42}"
TEST_SIZE="${TEST_SIZE:-0.2}"
HIDDEN_LAYERS="${HIDDEN_LAYERS:-64,32,16}"
MAX_ITER="${MAX_ITER:-2000}"

# ===== Helpers =====
die() { echo "ERRO: $*" >&2; exit 1; }

# ===== Check files =====
[ -f "train_model.py" ] || die "train_model.py não encontrado na raiz do repo. Coloque o arquivo na raiz e tente novamente."
[ -f "requirements.txt" ] || die "requirements.txt não encontrado na raiz do repo."

# ===== Create venv =====
if [ ! -d "$VENV_DIR" ]; then
  echo "[INFO] Criando venv em $VENV_DIR"
  "$PYTHON_BIN" -m venv "$VENV_DIR"
fi

# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"

echo "[INFO] Atualizando pip"
python -m pip install --upgrade pip >/dev/null

echo "[INFO] Instalando dependências"
pip install -r requirements.txt

# ===== Locate CSVs =====
# Preferência: Laboratorio/; fallback: Classificacao/
DATA_DIR=""
if [ -d "Laboratorio" ]; then
  DATA_DIR="Laboratorio"
elif [ -d "Classificacao" ]; then
  DATA_DIR="Classificacao"
else
  die "Não encontrei as pastas Laboratorio/ nem Classificacao/. Verifique a estrutura do repo."
fi

# Padrões por nome (ajuste se seus arquivos tiverem outro padrão)
# - Normais: *SemVazamento*.csv
# - Falha:   *ComVazamento*.csv
shopt -s nullglob
NORMAL_FILES=( "$DATA_DIR"/*SemVazamento*.csv )
FAULT_FILES=(  "$DATA_DIR"/*ComVazamento*.csv )
shopt -u nullglob

# Se não achou pelos padrões acima, tenta pegar "normal"/"fault" no nome (mais genérico)
if [ ${#NORMAL_FILES[@]} -eq 0 ]; then
  shopt -s nullglob
  NORMAL_FILES=( "$DATA_DIR"/*normal*.csv "$DATA_DIR"/*Normal*.csv )
  shopt -u nullglob
fi
if [ ${#FAULT_FILES[@]} -eq 0 ]; then
  shopt -s nullglob
  FAULT_FILES=( "$DATA_DIR"/*fault*.csv "$DATA_DIR"/*Fault*.csv "$DATA_DIR"/*vazamento*.csv "$DATA_DIR"/*Vazamento*.csv )
  shopt -u nullglob
fi

[ ${#NORMAL_FILES[@]} -gt 0 ] || die "Não encontrei CSVs de NORMAL em $DATA_DIR/ (procurei por *SemVazamento*.csv e variantes)."
[ ${#FAULT_FILES[@]}  -gt 0 ] || die "Não encontrei CSVs de FALHA em $DATA_DIR/ (procurei por *ComVazamento*.csv e variantes)."

echo "[INFO] Dataset dir: $DATA_DIR"
echo "[INFO] NORMAL CSVs:"
printf "  - %s\n" "${NORMAL_FILES[@]}"
echo "[INFO] FALHA CSVs:"
printf "  - %s\n" "${FAULT_FILES[@]}"

# ===== Run training =====
mkdir -p "$OUTDIR"

echo "[INFO] Rodando treino..."
python train_model.py \
  --normal "${NORMAL_FILES[@]}" \
  --fault  "${FAULT_FILES[@]}" \
  --outdir "$OUTDIR" \
  --seed "$SEED" \
  --test_size "$TEST_SIZE" \
  --hidden_layers "$HIDDEN_LAYERS" \
  --max_iter "$MAX_ITER" \
  --split_by_file

echo
echo "[OK] Treino finalizado. Artefatos gerados em: $OUTDIR/"
ls -la "$OUTDIR" | sed -n '1,200p'
