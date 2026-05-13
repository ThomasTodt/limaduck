#define DUCKDB_EXTENSION_MAIN

#include "duckteste_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

// Nossos cabeçalhos do LIMA
#include "Scheduler.hpp"
// #include "CSVDataset.hpp"
#include "DuckDBDataset.hpp"

namespace duckdb {

struct LimaDiscoverBindData : public TableFunctionData {
    string file_name;
    int32_t num_lines;
    double threshold;
};

// --- 1. O Estado Global ---
struct LimaDiscoverGlobalState : public GlobalTableFunctionState {
    // Usando o nosso CSVDataset temporariamente para teste
    std::unique_ptr<DuckDBDataset> dataset; 
    std::unique_ptr<Scheduler> scheduler;
    
    std::vector<std::string> discovered_constraints;
    idx_t result_idx = 0;
    bool finished_search = false;
};

// unique_ptr<FunctionData> LimaDiscoverBind(ClientContext &context, TableFunctionBindInput &input,
//                                          vector<LogicalType> &return_types, vector<string> &names) {
//     return_types.push_back(LogicalType::VARCHAR);
//     names.push_back("denial_constraint");
//     return make_uniq<TableFunctionData>(); 
// }

// --- 2. BIND (Captura os argumentos do SQL) ---
unique_ptr<FunctionData> LimaDiscoverBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
    auto result = make_uniq<LimaDiscoverBindData>();
    
    // 1. Argumento Obrigatório: Nome do Arquivo (Sempre o índice 0)
    result->file_name = input.inputs[0].GetValue<string>();

    // 2. Valores Padrão
    result->num_lines = -1; // -1 será o nosso código secreto para "Leia o arquivo inteiro"
    result->threshold = 0.0000001; // Padrão do Java: 0.00001 * 0.01 = 1e-7

    // 3. Verifica se o usuário passou o 'num_lines' opcional
    auto num_lines_it = input.named_parameters.find("num_lines");
    if (num_lines_it != input.named_parameters.end()) {
        result->num_lines = num_lines_it->second.GetValue<int32_t>();
    }

    // 4. Verifica se o usuário passou o 'threshold' opcional
    auto threshold_it = input.named_parameters.find("threshold");
    if (threshold_it != input.named_parameters.end()) {
        result->threshold = threshold_it->second.GetValue<double>();
    }

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("denial_constraint");
    return std::move(result);
}

// // --- 3. INIT (Carregamento) ---
// unique_ptr<GlobalTableFunctionState> LimaDiscoverInit(ClientContext &context, TableFunctionInitInput &input) {
//     auto state = make_uniq<LimaDiscoverGlobalState>();
    
//     fprintf(stderr, ">>> [LIMA] Lendo o arquivo CSV via C++...\n");
//     // HARDCODED temporário apenas para teste de sanidade
//     // Passamos o caminho, o número de linhas (10) e a seed (42)
//     state->dataset = make_uniq<CSVDataset>("test_small.csv", 10, 42);
    
//     fprintf(stderr, ">>> [LIMA] Instanciando o Scheduler...\n");
//     state->scheduler = make_uniq<Scheduler>(state->dataset.get());
    
//     return std::move(state);
// }

// --- 3. INIT (Injeta as variáveis no motor) ---
unique_ptr<GlobalTableFunctionState> LimaDiscoverInit(ClientContext &context, TableFunctionInitInput &input) {
    auto state = make_uniq<LimaDiscoverGlobalState>();
    auto &bind_data = (LimaDiscoverBindData &)*input.bind_data;
    
    fprintf(stderr, ">>> [LIMA] Arquivo: %s | Linhas: %d | Threshold: %e\n", 
            bind_data.file_name.c_str(), bind_data.num_lines, bind_data.threshold);
    
    // Passamos o caminho, o número de linhas dinâmico, e uma seed fixa 42
    // state->dataset = make_uniq<CSVDataset>(bind_data.file_name, bind_data.num_lines, 42);
    state->dataset = make_uniq<DuckDBDataset>(context, bind_data.file_name);

    fprintf(stderr, "dataset");
    
    // NOTA: Se você tiver uma variável global/estática para o minGrad no C++, 
    // defina ela aqui antes de criar o Scheduler! Ex:
    // Scheduler::minGrad = bind_data.threshold;
    
    state->scheduler = make_uniq<Scheduler>(state->dataset.get());

    fprintf(stderr, "scheduler");
    
    return std::move(state);
}

// --- 4. EXECUÇÃO ---
void LimaDiscoverFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = (LimaDiscoverGlobalState &)*data_p.global_state;
    
    if (!state.finished_search) {
        fprintf(stderr, ">>> [LIMA] Iniciando analise heuristica da Lattice...\n");
        
        // AQUI ESTÁ A MÁGICA LIGADA:
        state.scheduler->populatePredicates(state.discovered_constraints);
        
        state.finished_search = true;
        fprintf(stderr, ">>> [LIMA] Busca completa. Encontradas %zu restricoes.\n", state.discovered_constraints.size());
    }

    
    if (state.result_idx >= state.discovered_constraints.size()) {
        output.SetCardinality(0);
        return;
    }
    
    idx_t count = 0;
    auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
    
    while (count < STANDARD_VECTOR_SIZE && state.result_idx < state.discovered_constraints.size()) {
        result_ptr[count] = StringVector::AddString(output.data[0], state.discovered_constraints[state.result_idx]);
        count++;
        state.result_idx++;
    }

    fprintf(stderr, "antes cardinalidade");
    
    output.SetCardinality(count);
}

// --- 5. REGISTRO (Avisa o DuckDB sobre os argumentos) ---
void RegisterLimaDiscover(ExtensionLoader &loader) {
    // Registra a função com 1 argumento posicional (VARCHAR para o nome do arquivo)
    TableFunction lima_discover_fun("lima_discover", {LogicalType::VARCHAR}, 
                                    LimaDiscoverFunction, LimaDiscoverBind, LimaDiscoverInit);
    
    // Registra os parâmetros nomeados opcionais
    lima_discover_fun.named_parameters["num_lines"] = LogicalType::INTEGER;
    lima_discover_fun.named_parameters["threshold"] = LogicalType::DOUBLE;
    
    loader.RegisterFunction(lima_discover_fun);
}

static void LoadInternal(ExtensionLoader &loader) { RegisterLimaDiscover(loader); }
void DucktesteExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }
std::string DucktesteExtension::Name() { return "duckteste"; }
std::string DucktesteExtension::Version() const { return ""; }

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(duckteste, loader) { duckdb::LoadInternal(loader); }
}