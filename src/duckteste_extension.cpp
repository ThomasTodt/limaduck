#define DUCKDB_EXTENSION_MAIN

#include "duckteste_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

// Nossos cabeçalhos do LIMA
#include "Scheduler.hpp"
#include "CSVDataset.hpp"

namespace duckdb {

// --- 1. O Estado Global ---
struct LimaDiscoverGlobalState : public GlobalTableFunctionState {
    // Usando o nosso CSVDataset temporariamente para teste
    std::unique_ptr<CSVDataset> dataset; 
    std::unique_ptr<Scheduler> scheduler;
    
    std::vector<std::string> discovered_constraints;
    idx_t result_idx = 0;
    bool finished_search = false;
};

// --- 2. BIND ---
unique_ptr<FunctionData> LimaDiscoverBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("denial_constraint");
    return make_uniq<TableFunctionData>(); 
}

// --- 3. INIT (Carregamento) ---
unique_ptr<GlobalTableFunctionState> LimaDiscoverInit(ClientContext &context, TableFunctionInitInput &input) {
    auto state = make_uniq<LimaDiscoverGlobalState>();
    
    fprintf(stderr, ">>> [LIMA] Lendo o arquivo CSV via C++...\n");
    // HARDCODED temporário apenas para teste de sanidade
    // Passamos o caminho, o número de linhas (10) e a seed (42)
    state->dataset = make_uniq<CSVDataset>("test_small.csv", 10, 42);
    
    fprintf(stderr, ">>> [LIMA] Instanciando o Scheduler...\n");
    state->scheduler = make_uniq<Scheduler>(state->dataset.get());
    
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
    
    output.SetCardinality(count);
}

// --- 5. REGISTRO ---
void RegisterLimaDiscover(ExtensionLoader &loader) {
    TableFunction lima_discover_fun("lima_discover", {LogicalType::VARCHAR}, 
                                    LimaDiscoverFunction, LimaDiscoverBind, LimaDiscoverInit);
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