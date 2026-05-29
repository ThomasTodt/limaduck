import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import io

# 1. Entrada dos dados reais consolidados
actual_data = """scenario,engine,attrs,rows,threshold,time_s,mem_gb
time_x_data,DuckDB,20,10000,0.01,1.1225273609161377,0.3155326843261719
time_x_data,Java,20,10000,0.01,3.2732512950897217,0.20969772338867188
time_x_data,DuckDB,20,50000,0.01,1.286789894104004,0.2885704040527344
time_x_data,Java,20,50000,0.01,3.324018716812134,0.2036590576171875
time_x_data,DuckDB,20,100000,0.01,1.4903979301452637,0.29264068603515625
time_x_data,Java,20,100000,0.01,2.9702072143554688,0.21570205688476562
time_x_data,DuckDB,20,200000,0.01,1.9173123836517334,0.3001251220703125
time_x_data,Java,20,200000,0.01,3.5759153366088867,0.24915695190429688
time_x_data,DuckDB,20,499308,0.01,3.1352458000183105,0.3236579895019531
time_x_data,Java,20,499308,0.01,3.9290194511413574,0.6424789428710938
time_x_data,DuckDB,20,10000,0.0001,1.4354496002197266,0.35103607177734375
time_x_data,Java,20,10000,0.0001,3.727808713912964,0.4037818908691406
time_x_data,DuckDB,20,50000,0.0001,1.6064133644104004,0.2888679504394531
time_x_data,Java,20,50000,0.0001,3.979315996170044,0.3179588317871094
time_x_data,DuckDB,20,100000,0.0001,1.8232221603393555,0.2924690246582031
time_x_data,Java,20,100000,0.0001,3.0230603218078613,0.3450164794921875
time_x_data,DuckDB,20,200000,0.0001,2.1932363510131836,0.3006477355957031
time_x_data,Java,20,200000,0.0001,3.52516770362854,0.3682365417480469
time_x_data,DuckDB,20,499308,0.0001,3.5187013149261475,0.3233833312988281
time_x_data,Java,20,499308,0.0001,4.17714262008667,0.6528129577636719
time_x_data,DuckDB,20,10000,1e-06,2.787926435470581,0.3277931213378906
time_x_data,Java,20,10000,1e-06,5.541175603866577,1.1494140625
time_x_data,DuckDB,20,50000,1e-06,2.7638776302337646,0.3122367858886719
time_x_data,Java,20,50000,1e-06,5.58847451210022,1.1499557495117188
time_x_data,DuckDB,20,100000,1e-06,3.059276819229126,0.3168983459472656
time_x_data,Java,20,100000,1e-06,5.846230506896973,1.1257057189941406
time_x_data,DuckDB,20,200000,1e-06,3.697636604309082,0.3304290771484375
time_x_data,Java,20,200000,1e-06,5.640854120254517,1.0089645385742188
time_x_data,DuckDB,20,499308,1e-06,5.3359599113464355,0.3671875
time_x_data,Java,20,499308,1e-06,7.454289436340332,1.3236274719238281
time_x_data,DuckDB,20,10000,1e-08,13.61295461654663,0.4590034484863281
time_x_data,Java,20,10000,1e-08,19.289997816085815,1.1924095153808594
time_x_data,DuckDB,20,50000,1e-08,16.92227292060852,0.5440216064453125
time_x_data,Java,20,50000,1e-08,26.234314680099487,1.5746803283691406
time_x_data,DuckDB,20,100000,1e-08,19.35327410697937,0.5571746826171875
time_x_data,Java,20,100000,1e-08,23.964741706848145,1.2744102478027344
time_x_data,DuckDB,20,200000,1e-08,25.40318775177002,0.6709251403808594
time_x_data,Java,20,200000,1e-08,25.630948781967163,1.3982772827148438
time_x_data,DuckDB,20,499308,1e-08,22.99724769592285,0.5866355895996094
time_x_data,Java,20,499308,1e-08,31.070891618728638,1.6125450134277344
mem_x_data,DuckDB,20,5000,1e-08,9.960715293884277,0.3991508483886719
mem_x_data,Java,20,5000,1e-08,16.11093235015869,1.312713623046875
mem_x_data,DuckDB,20,15000,1e-08,16.63567852973938,0.5543136596679688
mem_x_data,Java,20,15000,1e-08,24.674097061157227,1.4064826965332031
mem_x_data,DuckDB,20,25000,1e-08,24.63296341896057,0.7135581970214844
mem_x_data,Java,20,25000,1e-08,21.654704093933105,1.4316673278808594
mem_x_data,DuckDB,20,35000,1e-08,27.275548219680786,0.7226333618164062
mem_x_data,Java,20,35000,1e-08,23.769323110580444,1.429962158203125
mem_x_data,DuckDB,20,45000,1e-08,26.336119413375854,0.7038841247558594
mem_x_data,Java,20,45000,1e-08,23.722177267074585,1.476043701171875
mem_x_data,DuckDB,20,55000,1e-08,18.219327926635742,0.5491828918457031
mem_x_data,Java,20,55000,1e-08,24.522459983825684,1.3280258178710938
mem_x_data,DuckDB,20,65000,1e-08,16.08647847175598,0.5391578674316406
mem_x_data,Java,20,65000,1e-08,30.113999366760254,1.6086502075195312
mem_x_data,DuckDB,20,75000,1e-08,18.64478826522827,0.5605506896972656
mem_x_data,Java,20,75000,1e-08,25.534611463546753,1.7322807312011719
mem_x_data,DuckDB,20,85000,1e-08,17.062464475631714,0.5535087585449219
mem_x_data,Java,20,85000,1e-08,23.42011833190918,1.4959144592285156
time_mem_x_preds,DuckDB,4,5000,1e-08,0.35777878761291504,0.0787200927734375
time_mem_x_preds,Java,4,5000,1e-08,0.6551175117492676,0.11326217651367188
time_mem_x_preds,DuckDB,8,5000,1e-08,0.8678934574127197,0.13020706176757812
time_mem_x_preds,Java,8,5000,1e-08,1.6112759113311768,0.2771186828613281
time_mem_x_preds,DuckDB,12,5000,1e-08,2.267411708831787,0.20863723754882812
time_mem_x_preds,Java,12,5000,1e-08,3.4764533042907715,0.5078201293945312
time_mem_x_preds,DuckDB,16,5000,1e-08,4.703244209289551,0.2959480285644531
time_mem_x_preds,Java,16,5000,1e-08,8.154604196548462,0.7629737854003906
time_mem_x_preds,DuckDB,20,5000,1e-08,9.842329263687134,0.3987083435058594
time_mem_x_preds,Java,20,5000,1e-08,16.516887187957764,1.4347305297851562"""

df = pd.read_csv(io.StringIO(actual_data))

# Configuração estética para artigos (estilo limpo acadêmico)
sns.set_theme(style="ticks")
plt.rcParams.update({
    'font.size': 11,
    'axes.labelsize': 12,
    'axes.titlesize': 13,
    'xtick.labelsize': 10,
    'ytick.labelsize': 10,
    'legend.fontsize': 10
})

# ==============================================================================
# GRAFICO 1: TIME X DATA (Linhas vs Tempo, quebrado por Threshold)
# ==============================================================================
plt.figure(figsize=(10, 5.5))
df_g1 = df[df['scenario'] == 'time_x_data'].copy()
df_g1['Engine (Threshold)'] = df_g1['engine'] + " ($\epsilon=" + df_g1['threshold'].astype(str) + "$)"

# Ordena para garantir consistência visual nas cores
df_g1 = df_g1.sort_values(by=['threshold', 'engine'], ascending=[False, True])

g1 = sns.lineplot(
    data=df_g1, x='rows', y='time_s', hue='Engine (Threshold)', 
    style='engine', markers=True, dashes=False, linewidth=2, palette="tab10"
)
g1.set_title("Execution Time vs. Row Scale (20 Attributes)", fontweight='bold', pad=15)
g1.set_xlabel("Number of Rows (Tuples)")
g1.set_ylabel("Execution Time (seconds)")
sns.despine()
plt.legend(bbox_to_anchor=(1.02, 1), loc='upper left', title="Configurations", frameon=True)
plt.tight_layout()
plt.savefig("1_actual_time_x_data.png", dpi=300)
plt.close()

# ==============================================================================
# GRAFICO 2: MEM X DATA (Evolução física do consumo de RAM)
# ==============================================================================
plt.figure(figsize=(7, 4.5))
df_g2 = df[df['scenario'] == 'mem_x_data']

g2 = sns.lineplot(
    data=df_g2, x='rows', y='mem_gb', hue='engine', 
    style='engine', markers=True, linewidth=2, palette=["#1f77b4", "#ff7f0e"]
)
g2.set_title("Memory Peak vs. Row Scale ($\epsilon=10^{-8}$, 20 Attributes)", fontweight='bold', pad=15)
g2.set_xlabel("Number of Rows (Tuples)")
g2.set_ylabel("Peak Memory Footprint (GB)")
g2.get_legend().set_title("Engine")
sns.despine()
plt.tight_layout()
plt.savefig("2_actual_mem_x_data.png", dpi=300)
plt.close()

# ==============================================================================
# GRAFICO 3: TIME X PREDS (Variação de complexidade por Atributos/Predicados)
# ==============================================================================
plt.figure(figsize=(7, 4.5))
df_g3 = df[df['scenario'] == 'time_mem_x_preds']

g3 = sns.lineplot(
    data=df_g3, x='attrs', y='time_s', hue='engine', 
    style='engine', markers=True, linewidth=2, palette=["#1f77b4", "#ff7f0e"]
)
g3.set_title("Execution Time vs. Predicate Space Complexity (@ 5k rows)", fontweight='bold', pad=15)
g3.set_xlabel("Number of Attributes (Predicate Space Scaling)")
g3.set_ylabel("Execution Time (seconds)")
g3.get_legend().set_title("Engine")
sns.despine()
plt.tight_layout()
plt.savefig("3_actual_time_x_preds.png", dpi=300)
plt.close()

# ==============================================================================
# GRAFICO 4: MEM X PREDS (Consumo de RAM conforme o espaço combinatório cresce)
# ==============================================================================
plt.figure(figsize=(7, 4.5))
df_g4 = df[df['scenario'] == 'time_mem_x_preds']

g4 = sns.lineplot(
    data=df_g4, x='attrs', y='mem_gb', hue='engine', 
    style='engine', markers=True, linewidth=2, palette=["#1f77b4", "#ff7f0e"]
)
g4.set_title("Memory Peak vs. Predicate Space Complexity (@ 5k rows)", fontweight='bold', pad=15)
g4.set_xlabel("Number of Attributes (Predicate Space Scaling)")
g4.set_ylabel("Peak Memory Footprint (GB)")
g4.get_legend().set_title("Engine")
sns.despine()
plt.tight_layout()
plt.savefig("4_actual_mem_x_preds.png", dpi=300)
plt.close()

print("Bateria de gráficos atualizada gerada com sucesso a partir dos dados reais!")