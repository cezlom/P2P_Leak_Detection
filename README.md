# SIMIG — P2P Leak Detection (ESP32 + AWS Lambda + IA)

Este repositório contém uma prova de conceito (PoC) para detecção e localização de vazamentos em rede (abordagem P2P), combinando:
- **Firmware ESP32** para aquisição/telemetria (`esp32.ino`)
- **Backend AWS Lambda + DynamoDB** para inferência e diagnóstico P2P (`lambda_function.py`)
- **Datasets CSV** organizados em `Laboratorio/` e/ou `Classificacao/`

Estrutura atual do repositório:
- `Laboratorio/`
- `Classificacao/`
- `esp32.ino`
- `lambda_function.py`
- `README.md` :contentReference[oaicite:2]{index=2}

---

## Visão geral da arquitetura

1. **ESP32 (edge)** coleta sinais (ex.: pressão/fluxo/temperatura) e publica telemetria (ex.: MQTT).
2. **AWS Lambda (cloud)** recebe eventos, calcula feature(s), executa inferência com modelo treinado e grava histórico no **DynamoDB**.
3. A Lambda também calcula um **diagnóstico P2P**: compara leituras recentes de múltiplos nós (sensores) para estimar **probabilidade de vazamento** e **epicentro** (origem provável).

---

## Componentes

### 1) Firmware — `esp32.ino`
- Responsável por aquisição, filtragem local (se aplicável), conectividade e envio de telemetria.
- Ajuste de pinos/sensores, rede e credenciais deve ser feito diretamente no sketch.

### 2) Backend — `lambda_function.py`
- Entrada: evento contendo `sensor_id` (ou `id`) + variáveis como `pressao`/`pressao_bar`/`p_out`, `f_out`, `temp_c`/`t1`.
- Processamento:
  - feature “wavelet” (ou equivalente, conforme pipeline adotado)
  - inferência (`model.joblib` + `scaler.joblib`)
  - diagnóstico P2P usando janela temporal recente em DynamoDB
- Saída: JSON com status, probabilidade e diagnóstico, e gravação no DynamoDB.

> **Importante:** para produção, recomenda-se evitar `Scan` em DynamoDB (custo/latência) e usar `Query` via chaves/índices (GSI), conforme modelagem do banco.

### 3) Notebook — `Simig_AI.ipynb`
- Exploração, leitura de CSVs e avaliação inicial do modelo.
- Recomendado migrar o treino final para um script reprodutível (`train_model.py`) quando for consolidar para defesa/produção.

---

## Datasets (CSV)

Os CSVs ficam em:
- `Laboratorio/` (recomendado para dados do laboratório)
- `Classificacao/` (alternativo/legado)

O treinamento normalmente usa:
- **Normal / Sem vazamento**
- **Falha / Com vazamento**

> Padronize nomes para facilitar automação, por exemplo:
- `*_SemVazamento*.csv`
- `*_ComVazamento*.csv`

---

## Treinamento reprodutível (recomendado)

Embora exista o notebook, o caminho “defensável” (banca/produção) é treinar via script com:
- feature engineering idêntico ao backend (evita *feature mismatch*)
- split por arquivo/sensor/tempo (evita *data leakage*)
- export de artefatos (`model.joblib`, `scaler.joblib`) + metadados/métricas

### 1) Pré-requisitos
Python 3.10+.

Crie `requirements.txt` (exemplo):
```txt
numpy>=1.24
pandas>=2.0
scikit-learn>=1.3
joblib>=1.3
PyWavelets>=1.4
