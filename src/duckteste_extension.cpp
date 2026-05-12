#define DUCKDB_EXTENSION_MAIN

#include "duckteste_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

// Aqui você inclui os cabeçalhos das classes poderosas que construímos
#include "Scheduler.hpp"
#include "RelationalDataset.hpp"
// #include "DuckDBDataset.hpp" // A classe que você fará para ler Chunks do DuckDB

namespace duckdb {

// --- 1. O Estado Global ---
// É aqui que o DuckDB guarda as variáveis enquanto a query está rodando.
struct LimaDiscoverGlobalState : public GlobalTableFunctionState {
    // std::unique_ptr<DuckDBDataset> dataset; // O seu adaptador de dados
    std::unique_ptr<Scheduler> scheduler;
    
    std::vector<std::string> discovered_constraints; // Guarda os resultados
    idx_t result_idx = 0; // Ponteiro de leitura para o output do DuckDB
    bool finished_search = false;
};

// --- 2. BIND (Validação Inicial) ---
unique_ptr<FunctionData> LimaDiscoverBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
    // Definimos que a tabela de saída terá apenas 1 coluna chamada "denial_constraint"
    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("denial_constraint");

    // Retornamos informações da tabela que o usuário digitou
    auto result = make_uniq<TableFunctionData>(); // Ajuste conforme sua estrutura de bind
    return std::move(result);
}

// --- 3. INIT (Carregamento de Dados) ---
unique_ptr<GlobalTableFunctionState> LimaDiscoverInit(ClientContext &context, TableFunctionInitInput &input) {
    auto state = make_uniq<LimaDiscoverGlobalState>();
    
    // AQUI ACONTECE A MÁGICA DE INGESTÃO:
    // Em vez de ler um arquivo CSV, você instancia o seu Dataset customizado
    // state->dataset = make_uniq<DuckDBDataset>();
    
    // (Lógica do DuckDB para ler as DataChunks da tabela e alimentar o seu state->dataset)
    // ...
    
    // Depois que os dados estiverem na memória colunar do LIMA, instanciamos o motor:
    // state->scheduler = make_uniq<Scheduler>(state->dataset.get());
    
    return std::move(state);
}

// --- 4. EXECUÇÃO (O Motor LIMA) ---
void LimaDiscoverFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = (LimaDiscoverGlobalState &)*data_p.global_state;
    
    // Se o LIMA ainda não rodou para esta query, nós rodamos agora
    if (!state.finished_search) {
        fprintf(stderr, ">>> [LIMA] Iniciando analise heuristica da Lattice...\n");
        
        // No seu Scheduler.cpp, em vez de dar cout, modifique o populatePredicates 
        // para receber o vetor e salvar os resultados nele!
        // state.scheduler->populatePredicates(state.discovered_constraints);
        
        state.finished_search = true;
        fprintf(stderr, ">>> [LIMA] Busca completa. Encontradas %zu restricoes.\n", state.discovered_constraints.size());
    }

    // Se já rodamos e mostramos tudo, avisamos ao DuckDB que acabou (Cardinalidade 0)
    if (state.result_idx >= state.discovered_constraints.size()) {
        output.SetCardinality(0);
        return;
    }

    // --- ENVIANDO OS RESULTADOS PARA O TERMINAL DO DUCKDB ---
    idx_t count = 0;
    auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
    
    // O DuckDB processa saídas em blocos (STANDARD_VECTOR_SIZE, geralmente 2048 linhas)
    while (count < STANDARD_VECTOR_SIZE && state.result_idx < state.discovered_constraints.size()) {
        // Pega a string do LIMA e converte para o formato interno de string do DuckDB
        result_ptr[count] = StringVector::AddString(output.data[0], state.discovered_constraints[state.result_idx]);
        
        count++;
        state.result_idx++;
    }
    
    // Dizemos ao DuckDB quantas linhas estamos enviando neste chunk
    output.SetCardinality(count);
}

// --- 5. REGISTRO DA EXTENSÃO ---
void RegisterLimaDiscover(ExtensionLoader &loader) {
    TableFunction lima_discover_fun("lima_discover", {LogicalType::VARCHAR}, 
                                    LimaDiscoverFunction, LimaDiscoverBind, LimaDiscoverInit);
    loader.RegisterFunction(lima_discover_fun);
}

static void LoadInternal(ExtensionLoader &loader) {
    RegisterLimaDiscover(loader);
}

void DucktesteExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string DucktesteExtension::Name() {
    return "duckteste";
}

std::string DucktesteExtension::Version() const {
#ifdef EXT_VERSION_DUCKTESTE
    return EXT_VERSION_DUCKTESTE;
#else
    return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(duckteste, loader) {
    duckdb::LoadInternal(loader);
}
}