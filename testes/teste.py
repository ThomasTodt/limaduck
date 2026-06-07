import duckdb
import time
import os
import psutil
import subprocess
import pandas as pd

# Configurações de Caminhos
EXTENSION_PATH = '/home/thomas/mestrado/mestrado_mockdb/mockdb/build/release/extension/lima/lima.duckdb_extension'
CSV_ORIGINAL = '/home/thomas/mestrado/DCValidity/datasets/flights.csv'
JAVA_JAR_PATH = '/home/thomas/mestrado/limajava/target/LIMA-0.1.jar'
TMP_DIR = '/home/thomas/mestrado/DCValidity/datasets/tmp_slices/'
DUCKDB_CLI_PATH = "/home/thomas/mestrado/mestrado_mockdb/mockdb/build/release/duckdb"

os.makedirs(TMP_DIR, exist_ok=True)

ALL_ATTRIBUTES = [
    '"Year(Integer)"', '"Quarter(Integer)"', '"Month(Integer)"', '"DayofMonth(Integer)"', 
    '"DayOfWeek(Integer)"', '"FlightDate(String)"', '"UniqueCarrier(String)"', '"AirlineID(Integer)"', 
    '"Carrier(String)"', '"TailNum(String)"', '"FlightNum(Integer)"', '"OriginAirportID(Integer)"', 
    '"OriginAirportSeqID(Integer)"', '"OriginCityMarketID(Integer)"', '"Origin(String)"', 
    '"OriginCityName(String)"', '"OriginState(String)"', '"OriginStateFips(Integer)"', 
    '"OriginStateName(String)"', '"OriginWac(Integer)"'
]

def get_memory_usage():
    process = psutil.Process(os.getpid())
    return process.memory_info().rss / (1024 ** 3) # GB

def run_duckdb_experiment_isolated(csv_path, num_lines, threshold, ext_path):
    # Cria um script SQL transiente para o CLI do DuckDB executar de forma isolada
    sql_script = f"""
    SET allow_unsigned_extensions=true;
    INSTALL '{ext_path}';
    LOAD 'lima';
    CREATE TABLE t_run AS SELECT * FROM read_csv_auto('{csv_path}');
    SELECT * FROM lima_discover('t_run', num_lines={num_lines}, threshold={threshold});
    """
    
    start_time = time.time()
    # Abre o DuckDB CLI como processo filho, injetando o SQL via stdin
    p = subprocess.Popen(
        [DUCKDB_CLI_PATH], 
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True
    )
    
    # Envia os comandos e monitora o RSS real do binário em C++
    peak_mem_bytes = 0
    try:
        proc = psutil.Process(p.pid)
        # Envia o script e deixa rodar enquanto faz o polling de memória
        p.stdin.write(sql_script)
        p.stdin.close()
        
        while p.poll() is None:
            try:
                mem = proc.memory_info().rss
                if mem > peak_mem_bytes:
                    peak_mem_bytes = mem
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
            time.sleep(0.01) # Polling mais agressivo (10ms) para C++
    except Exception as e:
        print(f"Erro no monitoramento C++: {e}")
        
    p.wait()
    elapsed = time.time() - start_time
    return elapsed, peak_mem_bytes / (1024 ** 3)

def run_duckdb_experiment(con, view_name, num_lines, threshold):
    start_mem = get_memory_usage()
    start_time = time.time()
    print(f"SELECT * FROM lima_discover('{view_name}', num_lines={num_lines}, threshold={threshold});")
    try:
        con.execute(f"SELECT * FROM lima_discover('{view_name}', num_lines={num_lines}, threshold={threshold});").fetchall()
    except Exception as e:
        print(f"Erro DuckDB: {e}")
        return None, None
    elapsed = time.time() - start_time
    peak_mem = max(0.0, get_memory_usage() - start_mem)
    return elapsed, peak_mem

def run_java_experiment(csv_path, num_lines, threshold):
    # Comando para chamar o JAR passando os parâmetros na ordem exata
    cmd = ["java", "-jar", JAVA_JAR_PATH, csv_path, str(threshold), str(num_lines)]
    
    start_time = time.time()
    
    # Popen permite monitorar o consumo de memória do processo filho em tempo real
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    
    peak_mem_bytes = 0
    try:
        proc = psutil.Process(p.pid)
        while p.poll() is None: # Enquanto o processo estiver rodando
            try:
                # Captura o RSS (Resident Set Size) do processo Java-----
                mem = proc.memory_info().rss
                if mem > peak_mem_bytes:
                    peak_mem_bytes = mem
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
            time.sleep(0.05) # Amostragem a cada 50ms
    except Exception as e:
        print(f"Erro no monitoramento Java: {e}")
        
    p.wait()
    elapsed = time.time() - start_time
    
    if p.returncode != 0:
        print(f"Erro na execução do Java (Exit Code {p.returncode})")
        return None, None
        
    peak_mem_gb = peak_mem_bytes / (1024 ** 3)
    return elapsed, peak_mem_gb



config = {
    "allow_unsigned_extensions": "true"
}

con = duckdb.connect(config=config)
con.execute(f"INSTALL '{EXTENSION_PATH}';")
con.execute("LOAD 'lima';")

print("Carregando dataset de origem no DuckDB...")
con.execute(f"CREATE TABLE flights_all AS SELECT * FROM read_csv_auto('{CSV_ORIGINAL}');")

results = []

# ==============================================================================
# 1. TIME X DATA (20 Atributos, Múltiplos Thresholds, Linhas Variáveis)
# ==============================================================================
print("\n--- 1/4 Iniciando Teste: Time x Data (DuckDB vs Java) ---")

csv_20_path = os.path.join(TMP_DIR, "flights_20_attrs.csv")
con.execute(f"COPY (SELECT {', '.join(ALL_ATTRIBUTES)} FROM flights_all) TO '{csv_20_path}' (HEADER, DELIMITER ',');")

thresholds = [10**-2, 10**-4, 10**-6, 10**-8]
row_scenarios_1 = [10000, 50000, 100000, 200000, 499308]

for t in thresholds:
    print(f"threshold {t}")
    for rows in row_scenarios_1:
        print(f"rows {rows}")
        # DuckDB
        # elapsed, mem = run_duckdb_experiment(con, view_20, rows, t)
        elapsed, mem = run_duckdb_experiment_isolated(csv_20_path, rows, t, EXTENSION_PATH)
        if elapsed is not None:
            results.append({"scenario": "time_x_data", "engine": "DuckDB", "attrs": 20, "rows": rows, "threshold": t, "time_s": elapsed, "mem_gb": mem})
        # Java
        elapsed, mem = run_java_experiment(csv_20_path, rows, t)
        if elapsed is not None:
            results.append({"scenario": "time_x_data", "engine": "Java", "attrs": 20, "rows": rows, "threshold": t, "time_s": elapsed, "mem_gb": mem})

os.remove(csv_20_path)

# ==============================================================================
# 2. MEM X DATA (20 Atributos, 5k .. 85k linhas)
# ==============================================================================
print("\n--- 2/4 Iniciando Teste: Mem x Data (DuckDB vs Java) ---")

csv_20_path = os.path.join(TMP_DIR, "flights_20_attrs.csv")
con.execute(f"COPY (SELECT {', '.join(ALL_ATTRIBUTES)} FROM flights_all) TO '{csv_20_path}' (HEADER, DELIMITER ',');")

row_scenarios_2 = list(range(5000, 86000, 10000))
fixed_threshold = 10**-8

for rows in row_scenarios_2:
    # DuckDB
    # elapsed, mem = run_duckdb_experiment(con, 'v_mem_data_20', rows, fixed_threshold)
    elapsed, mem = run_duckdb_experiment_isolated(csv_20_path, rows, fixed_threshold, EXTENSION_PATH)
    if elapsed is not None:
        results.append({"scenario": "mem_x_data", "engine": "DuckDB", "attrs": 20, "rows": rows, "threshold": fixed_threshold, "time_s": elapsed, "mem_gb": mem})
    # Java
    elapsed, mem = run_java_experiment(csv_20_path, rows, fixed_threshold)
    if elapsed is not None:
        results.append({"scenario": "mem_x_data", "engine": "Java", "attrs": 20, "rows": rows, "threshold": fixed_threshold, "time_s": elapsed, "mem_gb": mem})

os.remove(csv_20_path)

# ==============================================================================
# 3 & 4. TIME X PREDS & MEM X PREDS (5k linhas, Atributos Variáveis)
# ==============================================================================
print("\n--- 3/4 e 4/4 Iniciando Testes: Time e Mem x Predicates (DuckDB vs Java) ---")
attribute_slices = [4, 8, 12, 16, 20]
fixed_rows_preds = 5000

for count in attribute_slices:
    print(f"attributes {count}")
    selected_attrs = ALL_ATTRIBUTES[:count]
    attrs_str = ", ".join(selected_attrs)
    
    # view_name = f"v_preds_{count}"
    # con.execute(f"CREATE OR REPLACE TABLE {view_name} AS SELECT {attrs_str} FROM flights_all;")
    csv_slice_path = os.path.join(TMP_DIR, f"flights_{count}attr.csv")
    con.execute(f"COPY (SELECT {attrs_str} FROM flights_all) TO '{csv_slice_path}' (HEADER, DELIMITER ',');")
    
    # DuckDB
    # elapsed, mem = run_duckdb_experiment(con, view_name, fixed_rows_preds, fixed_threshold)
    elapsed, mem = run_duckdb_experiment_isolated(csv_slice_path, fixed_rows_preds, fixed_threshold, EXTENSION_PATH)
    if elapsed is not None:
        results.append({"scenario": "time_mem_x_preds", "engine": "DuckDB", "attrs": count, "rows": fixed_rows_preds, "threshold": fixed_threshold, "time_s": elapsed, "mem_gb": mem})
    
    # Java
    elapsed, mem = run_java_experiment(csv_slice_path, fixed_rows_preds, fixed_threshold)
    if elapsed is not None:
        results.append({"scenario": "time_mem_x_preds", "engine": "Java", "attrs": count, "rows": fixed_rows_preds, "threshold": fixed_threshold, "time_s": elapsed, "mem_gb": mem})

    os.remove(csv_slice_path)

# Exportação dos dados
df = pd.DataFrame(results)
df.to_csv("lima_completos_comparativos3.csv", index=False)
print("\nTodos os cenários foram executados e salvos!")