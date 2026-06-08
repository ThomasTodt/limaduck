import duckdb
import time
import os
import psutil
import subprocess
import pandas as pd
import numpy as np
# --- Configurações de Caminhos ---
EXTENSION_PATH = '/home/thomas/limaduck/mockdb/build/release/extension/lima/lima.duckdb_extension'
CSV_ORIGINAL = '/home/thomas/limaduck/DCValidity/datasets/flights.csv'
JAVA_JAR_PATH = '/home/thomas/limaduck/LIMA/target/LIMA-0.1.jar'
TMP_DIR = '/home/thomas/limaduck/DCValidity/datasets/tmp_slices/'
DUCKDB_CLI_PATH = "/home/thomas/limaduck/mockdb/build/release/duckdb"

NUM_RUNS = 10
os.makedirs(TMP_DIR, exist_ok=True)

ALL_ATTRIBUTES = [
    '"Year(Integer)"', '"Quarter(Integer)"', '"Month(Integer)"', '"DayofMonth(Integer)"', 
    '"DayOfWeek(Integer)"', '"FlightDate(String)"', '"UniqueCarrier(String)"', '"AirlineID(Integer)"', 
    '"Carrier(String)"', '"TailNum(String)"', '"FlightNum(Integer)"', '"OriginAirportID(Integer)"', 
    '"OriginAirportSeqID(Integer)"', '"OriginCityMarketID(Integer)"', '"Origin(String)"', 
    '"OriginCityName(String)"', '"OriginState(String)"', '"OriginStateFips(Integer)"', 
    '"OriginStateName(String)"', '"OriginWac(Integer)"'
]

def run_duckdb_experiment_isolated(csv_path, num_lines, threshold, ext_path):
    sql_script = f"""
    SET allow_unsigned_extensions=true;
    INSTALL '{ext_path}';
    LOAD 'lima';
    CREATE TABLE t_run AS SELECT * FROM read_csv_auto('{csv_path}');
    SELECT * FROM lima_discover('t_run', num_lines={num_lines}, threshold={threshold});
    """
    
    start_time = time.time()
    p = subprocess.Popen(
        [DUCKDB_CLI_PATH], 
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        text=True
    )
    
    peak_mem_bytes = 0
    try:
        proc = psutil.Process(p.pid)
        p.stdin.write(sql_script)
        p.stdin.close()
        
        while p.poll() is None:
            try:
                mem = proc.memory_info().rss
                if mem > peak_mem_bytes:
                    peak_mem_bytes = mem
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                break
            time.sleep(0.01) 
    except Exception as e:
        print(f"Erro no monitoramento C++: {e}")
        
    p.wait()
    elapsed = time.time() - start_time
    return elapsed, peak_mem_bytes / (1024 ** 3)

def run_java_experiment(csv_path, num_lines, threshold):
    cmd = ["java", "-jar", JAVA_JAR_PATH, csv_path, str(threshold), str(num_lines)]
    start_time = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    peak_mem_bytes = 0
    try:
        proc = psutil.Process(p.pid)
        while p.poll() is None:
            try:
                mem = proc.memory_info().rss
                if mem > peak_mem_bytes:
                    peak_mem_bytes = mem
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                break
            time.sleep(0.01)
    except Exception as e:
        print(f"Erro no monitoramento Java: {e}")
        
    p.wait()
    elapsed = time.time() - start_time
    
    if p.returncode != 0:
        return None, None
        
    return elapsed, peak_mem_bytes / (1024 ** 3)

def run_benchmark_with_averages(engine, run_func, *args):
    """Executa o experimento NUM_RUNS vezes e retorna a média de tempo e memória."""
    times = []
    mems = []
    
    for i in range(NUM_RUNS):
        print(f"      Run {i+1}/{NUM_RUNS}...")
        elapsed, mem = run_func(*args)
        if elapsed is not None:
            times.append(elapsed)
            mems.append(mem)
            
    print(" " * 40) # Limpa a linha do status de execução
    if not times:
        return None, None
        
    return np.mean(times), np.mean(mems)

# --- Setup Inicial ---
config = {"allow_unsigned_extensions": "true"}
con = duckdb.connect(config=config)
print("Carregando dataset de origem no DuckDB...")
con.execute(f"CREATE TABLE flights_all AS SELECT * FROM read_csv_auto('{CSV_ORIGINAL}');")

results = []

# ==============================================================================
# 1. TIME X DATA
# ==============================================================================
print("\n--- 1/4 Iniciando Teste: Time x Data (Média de 10 execuções) ---")
csv_20_path = os.path.join(TMP_DIR, "flights_20_attrs.csv")
con.execute(f"COPY (SELECT {', '.join(ALL_ATTRIBUTES)} FROM flights_all) TO '{csv_20_path}' (HEADER, DELIMITER ',');")

thresholds = [10**-2, 10**-4, 10**-6, 10**-8]
row_scenarios_1 = [10000, 50000, 100000, 200000, 499308]

for t in thresholds:
    print(f"\nScenario: Threshold={t}")
    for rows in row_scenarios_1:
        print(f"   Rows: {rows:7} | ", end="")
        
        # DuckDB
        print("DuckDB...", end="")
        avg_time, avg_mem = run_benchmark_with_averages("DuckDB", run_duckdb_experiment_isolated, csv_20_path, rows, t, EXTENSION_PATH)
        if avg_time:
            results.append({"scenario": "time_x_data", "engine": "DuckDB", "attrs": 20, "rows": rows, "threshold": t, "time_s": avg_time, "mem_gb": avg_mem})
            print(f"Avg: {avg_time:6.2f}s", end=" | ")
            
        # Java
        print("Java...", end="")
        avg_time, avg_mem = run_benchmark_with_averages("Java", run_java_experiment, csv_20_path, rows, t)
        if avg_time:
            results.append({"scenario": "time_x_data", "engine": "Java", "attrs": 20, "rows": rows, "threshold": t, "time_s": avg_time, "mem_gb": avg_mem})
            print(f"Avg: {avg_time:6.2f}s")

os.remove(csv_20_path)

# ==============================================================================
# 2. MEM X DATA
# ==============================================================================
print("\n--- 2/4 Iniciando Teste: Mem x Data (Média de 10 execuções) ---")
csv_20_path = os.path.join(TMP_DIR, "flights_20_attrs.csv")
con.execute(f"COPY (SELECT {', '.join(ALL_ATTRIBUTES)} FROM flights_all) TO '{csv_20_path}' (HEADER, DELIMITER ',');")

row_scenarios_2 = list(range(5000, 86000, 10000))
fixed_threshold = 10**-8

for rows in row_scenarios_2:
    print(f"   Rows: {rows:7} | ", end="")
    
    # DuckDB
    print("DuckDB...", end="")
    avg_time, avg_mem = run_benchmark_with_averages("DuckDB", run_duckdb_experiment_isolated, csv_20_path, rows, fixed_threshold, EXTENSION_PATH)
    if avg_time:
        results.append({"scenario": "mem_x_data", "engine": "DuckDB", "attrs": 20, "rows": rows, "threshold": fixed_threshold, "time_s": avg_time, "mem_gb": avg_mem})
        print(f"Avg Mem: {avg_mem:4.2f}GB", end=" | ")
        
    # Java
    print("Java...", end="")
    avg_time, avg_mem = run_benchmark_with_averages("Java", run_java_experiment, csv_20_path, rows, fixed_threshold)
    if avg_time:
        results.append({"scenario": "mem_x_data", "engine": "Java", "attrs": 20, "rows": rows, "threshold": fixed_threshold, "time_s": avg_time, "mem_gb": avg_mem})
        print(f"Avg Mem: {avg_mem:4.2f}GB")

os.remove(csv_20_path)

# ==============================================================================
# 3 & 4. TIME X PREDS & MEM X PREDS
# ==============================================================================
print("\n--- 3/4 e 4/4 Iniciando Testes: Time e Mem x Preds (Média de 10 execuções) ---")
attribute_slices = [4, 8, 12, 16, 20]
fixed_rows_preds = 5000

for count in attribute_slices:
    print(f"   Attributes: {count:2} | ", end="")
    selected_attrs = ALL_ATTRIBUTES[:count]
    attrs_str = ", ".join(selected_attrs)
    csv_slice_path = os.path.join(TMP_DIR, f"flights_{count}attr.csv")
    con.execute(f"COPY (SELECT {attrs_str} FROM flights_all) TO '{csv_slice_path}' (HEADER, DELIMITER ',');")
    
    # DuckDB
    print("DuckDB...", end="")
    avg_time, avg_mem = run_benchmark_with_averages("DuckDB", run_duckdb_experiment_isolated, csv_slice_path, fixed_rows_preds, fixed_threshold, EXTENSION_PATH)
    if avg_time:
        results.append({"scenario": "time_mem_x_preds", "engine": "DuckDB", "attrs": count, "rows": fixed_rows_preds, "threshold": fixed_threshold, "time_s": avg_time, "mem_gb":avg_mem})
        print(f"Avg: {avg_time:6.2f}s", end=" | ")
    
    # Java
    print("Java...", end="")
    avg_time, avg_mem = run_benchmark_with_averages("Java", run_java_experiment, csv_slice_path, fixed_rows_preds, fixed_threshold)
    if avg_time:
        results.append({"scenario": "time_mem_x_preds", "engine": "Java", "attrs": count, "rows": fixed_rows_preds, "threshold": fixed_threshold, "time_s": avg_time, "mem_gb":avg_mem})
        print(f"Avg: {avg_time:6.2f}s")

    os.remove(csv_slice_path)

# Exportação final
pd.DataFrame(results).to_csv("lima_completos_comparativos_final.csv", index=False)
print("\nTodos os cenários foram executados 10 vezes. Médias salvas com sucesso")