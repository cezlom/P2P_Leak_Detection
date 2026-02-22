import json
import numpy as np
import pywt
import joblib
import os
import boto3
from datetime import datetime, timedelta
from decimal import Decimal
from boto3.dynamodb.conditions import Key

# --- CONFIGURAÇÕES ---
BASE_PATH = os.environ.get('LAMBDA_TASK_ROOT', '.')
MODEL_PATH = os.path.join(BASE_PATH, 'model.joblib')
SCALER_PATH = os.path.join(BASE_PATH, 'scaler.joblib')

dynamodb = boto3.resource('dynamodb')
table = dynamodb.Table('Historico_SIMIG')

def load_ai_assets():
    try:
        m = joblib.load(MODEL_PATH)
        s = joblib.load(SCALER_PATH)
        return m, s
    except:
        return None, None

model, scaler = load_ai_assets()

def convert_floats_to_decimals(obj):
    if isinstance(obj, float): return Decimal(str(obj))
    elif isinstance(obj, dict): return {k: convert_floats_to_decimals(v) for k, v in obj.items()}
    elif isinstance(obj, list): return [convert_floats_to_decimals(i) for i in obj]
    return obj

# --- MOTOR DE PROBABILIDADE E LOCALIZAÇÃO ---
def calcular_diagnostico_rede(id_atual, p_atual, w_atual, f_atual):
    """
    Compara os dados atuais com os vizinhos para calcular a probabilidade
    de o vazamento ser NESTE sensor específico (Epicentro).
    """
    try:
        # 1. Busca vizinhos nos últimos 2 minutos
        agora = datetime.utcnow()
        janela = (agora - timedelta(minutes=2)).isoformat()
        
        response = table.scan(FilterExpression=Key('timestamp').gt(janela))
        items = response.get('Items', [])
        
        if not items:
            return 0, "DADO ISOLADO"

        # 2. Mapeia a rede atual
        rede = {item['sensor_id']: float(item['pressao']) for item in items}
        rede[id_atual] = p_atual
        
        # 3. CÁLCULO DO SCORE DE PROBABILIDADE (0 a 100%)
        score = 0
        
        # Fator A: Queda de Pressão (Delta P em relação ao ideal de 5.0)
        delta_p = max(0, 5.0 - p_atual)
        score += delta_p * 15  # Se cair 3 bar, ganha 45% de probabilidade
        
        # Fator B: Energia Wavelet (Turbulência)
        score += abs(w_atual) * 10 # Se a wavelet for 2.0, ganha 20%
        
        # Fator C: Fluxo Anômalo (O Pulo do Gato)
        # Se houver fluxo > 0.5 L/min no sistema enquanto a pressão cai
        if f_atual > 0.5:
            score += 25 # Bônus de 25% na certeza de que é vazamento
            
        # Fator D: Comparativo de Epicentro
        p_min_rede = min(rede.values())
        if p_atual <= p_min_rede + 0.05: # Se eu sou o menor (ou quase o menor)
            score += 15 # Bônus por ser o ponto de menor potencial
            
        # Limita o score em 99%
        probabilidade = min(99, round(score, 1))
        
        # 4. DEFINIÇÃO DA ORIGEM
        id_epicentro = min(rede, key=rede.get)
        if id_epicentro == id_atual:
            localizacao = "EPICENTRO CONFIRMADO"
        else:
            localizacao = f"REFLEXO (Origem: {id_epicentro})"
            
        return probabilidade, localizacao

    except Exception as e:
        print(f"Erro no calculo: {e}")
        return 0, "ERRO SENSORIAL"

def lambda_handler(event, context):
    ts = datetime.utcnow().isoformat()
    raw_id = event.get('sensor_id') or event.get('id', 'N/A')
    
    # Extração Multivariada
    p_lida = float(event.get('pressao', event.get('pressao_bar', event.get('p_out', 5.0))))
    f_lida = float(event.get('f_out', 0.0))
    t_lida = float(event.get('temp_c', event.get('t1', 0.0)))

    # Cálculo da Wavelet
    hist = np.full(9, 5.0)
    sinal = np.append(hist, p_lida)
    cA, cD = pywt.dwt(sinal, 'db1')
    w = float(cD[-1])

    # Detecção de Falha (IA Local)
    st_ia = "NORMAL"
    if model:
        try:
            inp = scaler.transform([[p_lida, w]])
            st_ia = "FALHA" if model.predict(inp)[0] == 1 else "NORMAL"
        except: pass

    # --- DIAGNÓSTICO DE REDE (PROBABILIDADE) ---
    prob, loc = 0, "AGUARDANDO"
    if p_lida < 4.5 or w > 0.2: # Só calcula se houver sinal de distúrbio
        prob, loc = calcular_diagnostico_rede(raw_id, p_lida, w, f_lida)

    # Objeto de Resposta
    item = {
        'sensor_id': raw_id,
        'timestamp': ts,
        'pressao': p_lida,
        'fluxo': f_lida,
        'temperatura': t_lida,
        'wavelet': w,
        'status_ia': st_ia,
        'probabilidade_vazamento_pct': prob,
        'diagnostico_localizacao': loc
    }
    
    table.put_item(Item=convert_floats_to_decimals(item))
    return {'statusCode': 200, 'body': item}
