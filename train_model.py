#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
train_model.py — Treinamento reprodutível do classificador SIMIG (Pressão + Wavelet)

Objetivo (compatível com a Lambda):
- Features exatamente como na inferência:
  X = [pressao_bar, w]
  w = cD[-1] de DWT db1 aplicada em: [P_IDEAL]*HIST_LEN + [pressao_bar]
- Escalonamento (StandardScaler) + MLPClassifier
- Split defensável (por arquivo/sensor, evitando leakage de série temporal)
- Exporta:
  - model.joblib
  - scaler.joblib
  - metadata.json
  - metrics.json

Exemplo:
python train_model.py \
  --normal Laboratorio/ESP01_SemVazamento.csv Laboratorio/ESP04_SemVazamento.csv \
  --fault  Laboratorio/ESP01_ComVazamento.csv Laboratorio/ESP04_ComVazamento.csv \
  --outdir artifacts \
  --split_by_file

Observação:
- Este script recalcula a wavelet a partir da PRESSÃO e IGNORA qualquer coluna "Wavelet" existente no CSV,
  para garantir consistência treino↔inferência.
"""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass, asdict
from datetime import datetime
from typing import List, Tuple, Dict, Optional

import joblib
import numpy as np
import pandas as pd
import pywt
from sklearn.metrics import (
    classification_report,
    confusion_matrix,
    accuracy_score,
    f1_score,
    precision_score,
    recall_score,
    roc_auc_score,
)
from sklearn.model_selection import train_test_split
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler


# -----------------------------
# Config / Metadata
# -----------------------------

@dataclass
class PipelineSignature:
    # Feature engineering (deve bater com a Lambda)
    wavelet_name: str = "db1"
    hist_len: int = 9
    p_ideal: float = 5.0
    wavelet_feature: str = "cD[-1]"
    feature_order: Tuple[str, str] = ("pressao_bar", "wavelet")

    # Thresholds não afetam treino (só diagnóstico), mas documente se quiser
    notes: str = "Wavelet calculada como DWT db1 em [p_ideal]*hist_len + [pressao] e usa cD[-1]."


# -----------------------------
# Feature engineering (igual Lambda)
# -----------------------------

def wavelet_feature_db1(pressao: float, p_ideal: float, hist_len: int) -> float:
    hist = np.full(hist_len, p_ideal, dtype=float)
    sinal = np.append(hist, float(pressao))
    _cA, cD = pywt.dwt(sinal, "db1")
    return float(cD[-1])


def infer_pressure_column(df: pd.DataFrame) -> str:
    """
    Tenta mapear automaticamente o nome da coluna de pressão.
    Ajuste aqui caso seus CSVs usem outro padrão.
    """
    candidates = [
        "Pressao", "pressao", "pressao_bar", "p_out", "pressure", "Pressure"
    ]
    for c in candidates:
        if c in df.columns:
            return c
    raise ValueError(f"Nenhuma coluna de pressão encontrada. Colunas disponíveis: {list(df.columns)}")


def read_csvs(paths: List[str], label: int) -> pd.DataFrame:
    frames = []
    for p in paths:
        df = pd.read_csv(p)
        df["__source_file__"] = os.path.basename(p)
        df["Target"] = int(label)
        frames.append(df)
    return pd.concat(frames, ignore_index=True)


def build_dataset(normal_paths: List[str], fault_paths: List[str], sig: PipelineSignature) -> pd.DataFrame:
    df_n = read_csvs(normal_paths, label=0)
    df_f = read_csvs(fault_paths, label=1)
    df = pd.concat([df_n, df_f], ignore_index=True)

    # Pressão
    p_col = infer_pressure_column(df)
    df["pressao_bar"] = pd.to_numeric(df[p_col], errors="coerce")

    # Remove inválidos
    df = df.dropna(subset=["pressao_bar"]).copy()

    # Wavelet recalculada (NÃO confia na coluna do CSV)
    df["wavelet"] = df["pressao_bar"].apply(lambda p: wavelet_feature_db1(p, sig.p_ideal, sig.hist_len))

    # Features finais
    df["__x0__"] = df["pressao_bar"].astype(float)
    df["__x1__"] = df["wavelet"].astype(float)

    return df


# -----------------------------
# Split defensável
# -----------------------------

def split_by_file(df: pd.DataFrame, test_size: float, seed: int) -> Tuple[pd.DataFrame, pd.DataFrame]:
    """
    Split por arquivo: deixa arquivos inteiros fora do treino (reduz leakage).
    """
    files = df["__source_file__"].unique().tolist()
    if len(files) < 2:
        raise ValueError("Split por arquivo requer pelo menos 2 arquivos diferentes.")

    train_files, test_files = train_test_split(files, test_size=test_size, random_state=seed, shuffle=True)
    df_train = df[df["__source_file__"].isin(train_files)].copy()
    df_test = df[df["__source_file__"].isin(test_files)].copy()
    return df_train, df_test


def split_random(df: pd.DataFrame, test_size: float, seed: int) -> Tuple[pd.DataFrame, pd.DataFrame]:
    """
    Split aleatório (use só se você não tiver séries temporais correlacionadas, ou se estiver ciente do risco).
    """
    df_train, df_test = train_test_split(df, test_size=test_size, random_state=seed, shuffle=True, stratify=df["Target"])
    return df_train.copy(), df_test.copy()


# -----------------------------
# Treino / Avaliação
# -----------------------------

def train_model(
    df_train: pd.DataFrame,
    df_test: pd.DataFrame,
    seed: int,
    hidden_layers: Tuple[int, ...],
    max_iter: int,
) -> Dict[str, object]:
    X_train = df_train[["__x0__", "__x1__"]].to_numpy(dtype=float)
    y_train = df_train["Target"].to_numpy(dtype=int)

    X_test = df_test[["__x0__", "__x1__"]].to_numpy(dtype=float)
    y_test = df_test["Target"].to_numpy(dtype=int)

    scaler = StandardScaler()
    X_train_s = scaler.fit_transform(X_train)
    X_test_s = scaler.transform(X_test)

    model = MLPClassifier(
        hidden_layer_sizes=hidden_layers,
        activation="relu",
        solver="adam",
        alpha=0.0001,
        max_iter=max_iter,
        random_state=seed,
        early_stopping=True,
        n_iter_no_change=20,
    )
    model.fit(X_train_s, y_train)

    y_pred = model.predict(X_test_s)

    # Probabilidade (para AUC)
    auc = None
    if hasattr(model, "predict_proba"):
        y_proba = model.predict_proba(X_test_s)[:, 1]
        try:
            auc = float(roc_auc_score(y_test, y_proba))
        except Exception:
            auc = None

    metrics = {
        "n_train": int(len(df_train)),
        "n_test": int(len(df_test)),
        "accuracy": float(accuracy_score(y_test, y_pred)),
        "precision": float(precision_score(y_test, y_pred, zero_division=0)),
        "recall": float(recall_score(y_test, y_pred, zero_division=0)),
        "f1": float(f1_score(y_test, y_pred, zero_division=0)),
        "roc_auc": auc,
        "confusion_matrix": confusion_matrix(y_test, y_pred).tolist(),
        "classification_report": classification_report(y_test, y_pred, zero_division=0),
    }

    return {
        "model": model,
        "scaler": scaler,
        "metrics": metrics,
    }


# -----------------------------
# CLI / Main
# -----------------------------

def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--normal", nargs="+", required=True, help="CSV(s) de operação normal (Target=0)")
    ap.add_argument("--fault", nargs="+", required=True, help="CSV(s) com falha/vazamento (Target=1)")
    ap.add_argument("--outdir", default="artifacts", help="Pasta de saída para model/scaler/metadata")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--test_size", type=float, default=0.2)

    # Compatibilidade com Lambda
    ap.add_argument("--p_ideal", type=float, default=5.0)
    ap.add_argument("--hist_len", type=int, default=9)
    ap.add_argument("--wavelet", default="db1", help="Mantido por documentação; a feature implementada usa db1.")

    # Split strategy
    ap.add_argument("--split_by_file", action="store_true", help="Recomendado: split por arquivo/sensor.")
    ap.add_argument("--split_random", action="store_true", help="Split aleatório (menos defensável em série temporal).")

    # Modelo
    ap.add_argument("--hidden_layers", default="64,32,16", help="Ex: 64,32,16")
    ap.add_argument("--max_iter", type=int, default=2000)

    return ap.parse_args()


def main():
    args = parse_args()

    # Estratégia de split: padrão = por arquivo (se possível)
    if not args.split_by_file and not args.split_random:
        args.split_by_file = True

    # Signature do pipeline
    sig = PipelineSignature(
        wavelet_name="db1",
        hist_len=int(args.hist_len),
        p_ideal=float(args.p_ideal),
        wavelet_feature="cD[-1]",
        feature_order=("pressao_bar", "wavelet"),
        notes="Treino recalcula wavelet para garantir consistência com a inferência na Lambda.",
    )

    # Dataset
    df = build_dataset(args.normal, args.fault, sig)

    # Split
    if args.split_by_file:
        df_train, df_test = split_by_file(df, test_size=args.test_size, seed=args.seed)
    else:
        df_train, df_test = split_random(df, test_size=args.test_size, seed=args.seed)

    # Modelo params
    hidden_layers = tuple(int(x.strip()) for x in args.hidden_layers.split(",") if x.strip())

    result = train_model(
        df_train=df_train,
        df_test=df_test,
        seed=args.seed,
        hidden_layers=hidden_layers,
        max_iter=args.max_iter,
    )

    # Saída
    os.makedirs(args.outdir, exist_ok=True)
    model_path = os.path.join(args.outdir, "model.joblib")
    scaler_path = os.path.join(args.outdir, "scaler.joblib")
    metadata_path = os.path.join(args.outdir, "metadata.json")
    metrics_path = os.path.join(args.outdir, "metrics.json")

    joblib.dump(result["model"], model_path)
    joblib.dump(result["scaler"], scaler_path)

    metadata = {
        "created_utc": datetime.utcnow().isoformat(),
        "pipeline_signature": asdict(sig),
        "training": {
            "normal_files": [os.path.basename(p) for p in args.normal],
            "fault_files": [os.path.basename(p) for p in args.fault],
            "split_strategy": "by_file" if args.split_by_file else "random_stratified",
            "test_size": float(args.test_size),
            "seed": int(args.seed),
        },
        "model": {
            "type": type(result["model"]).__name__,
            "hidden_layers": list(hidden_layers),
            "max_iter": int(args.max_iter),
        },
        "features": {
            "X_order": ["pressao_bar", "wavelet"],
            "wavelet_computation": {
                "method": "pywt.dwt",
                "wavelet": "db1",
                "signal": f"[{sig.p_ideal}]*{sig.hist_len} + [pressao_bar]",
                "output_used": "cD[-1]",
            },
        },
    }

    with open(metadata_path, "w", encoding="utf-8") as f:
        json.dump(metadata, f, ensure_ascii=False, indent=2)

    with open(metrics_path, "w", encoding="utf-8") as f:
        json.dump(result["metrics"], f, ensure_ascii=False, indent=2)

    # Logs essenciais (para relatório/dissertação)
    print("\n=== Treino concluído ===")
    print(f"Model:   {model_path}")
    print(f"Scaler:  {scaler_path}")
    print(f"Meta:    {metadata_path}")
    print(f"Metrics: {metrics_path}\n")

    print("=== Métricas (teste) ===")
    for k in ["n_train", "n_test", "accuracy", "precision", "recall", "f1", "roc_auc"]:
        print(f"{k}: {result['metrics'].get(k)}")
    print("\nConfusion matrix:")
    print(np.array(result["metrics"]["confusion_matrix"]))
    print("\nClassification report:")
    print(result["metrics"]["classification_report"])


if __name__ == "__main__":
    main()
