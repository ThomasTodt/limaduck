#define DUCKDB_EXTENSION_MAIN

#include "duckteste_extension.hpp"
#include "lima_structures.hpp"
#include "duckdb.hpp"
#include <random>
#include <unordered_map>
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
// #include "duckdb/main/extension_util.hpp" // Necessário para ExtensionUtil
#include "duckdb/main/database.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/common/types/column/column_data_collection_iterators.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include <optional>
#include <thread>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>
#include <cstdio>

namespace duckdb {

inline void DucktesteScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Duckteste " + name.GetString() + " 🐥");
	});
}

inline void DucktesteBinaryFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &a = args.data[0];
	auto &b = args.data[1];
	BinaryExecutor::Execute<string_t, string_t, string_t>(a, b, result, args.size(), [&](string_t left, string_t right) {
		return StringVector::AddString(result, left.GetString() + " & " + right.GetString());
	});
}

inline void DucktesteOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Duckteste " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

struct SumSquaresState {
    double total;
};

struct SumSquaresOperation {
    using STATE = SumSquaresState;
    using INPUT_TYPE = double;
    using RESULT_TYPE = double;

    // engine calls this to check whether to skip null checks fast-path
    static bool IgnoreNull() {
        return true; // skip nulls (do not feed nulls to Operation)
    }

	static void Initialize(STATE &state) {
        state.total = 0.0;
    }

    // Called when input vector is a constant vector (one value for whole batch)
    template <class INPUT_T, class STATE_T, class OP>
    static void ConstantOperation(STATE_T &state, INPUT_T input, duckdb::AggregateUnaryInput &input_data, idx_t count) {
        // input is unwrapped to INPUT_T already
        state.total += (double)input * (double)input;
    }

    // Per-element update path used by the generic executor
    template <class INPUT_T, class STATE_T, class OP>
    static void Operation(STATE_T &state, INPUT_T input, duckdb::AggregateUnaryInput &input_data) {
        state.total += (double)input * (double)input;
    }

    // Merge partial states (source -> target)
    template <class STATE_T, class OP>
    static void Combine(const STATE_T &source, STATE_T &target, duckdb::AggregateInputData &aggr_input) {
        target.total += source.total;
    }

    // Finalize: engine expects Finalize<RESULT_T, STATE_T>(STATE_T&, RESULT_T&, AggregateFinalizeData&)
    template <class RESULT_T, class STATE_T>
    static void Finalize(const STATE_T &state, RESULT_T &result, duckdb::AggregateFinalizeData &finalize_data) {
        result = (RESULT_T)state.total;
    }
};


void RegisterSumSquares(AggregateFunctionSet &set) {
    auto input_type = LogicalType::DOUBLE;
    auto result_type = LogicalType::DOUBLE;

    auto fun = AggregateFunction::UnaryAggregate<SumSquaresState, double, double, SumSquaresOperation>(
        input_type, result_type
    );

    set.AddFunction(fun);
}


// TABLE FUNCTION

struct EvidenceSetBindData : public TableFunctionData {
    // Aqui você guarda metadados, como nomes de colunas
    // vector<LogicalType> return_types;
    string table_name;
    vector<string> table_column_names;
    vector<LogicalType> table_types;
    bool use_hashing = false;
    bool use_vectorization = false;
};

struct EvidenceSetGlobalState : public GlobalTableFunctionState {
    
    idx_t total_rows = 0;
    
    shared_ptr<ColumnDataCollection> collection;
    atomic<idx_t> current_row_i;

    vector<hash_t> row_hashes;

    EvidenceSetGlobalState(idx_t rows, ClientContext &context, const vector<LogicalType> &types) 
        : total_rows(rows), current_row_i(0) {
        collection = duckdb::make_shared_ptr<ColumnDataCollection>(context, types);
    }

    idx_t MaxThreads() const override {
        // Retorne um número alto ou baseado no total de linhas para permitir escalonamento
        return 64; 
    }
};

struct EvidenceSetLocalState : public LocalTableFunctionState {
    DataChunk chunk_i;
    DataChunk chunk_j;
    // Buffer para evitar realocações de string constantes
    string diff_buffer;

    idx_t i_start = 0;
    idx_t i_end = 0;
    idx_t current_i = (idx_t)-1; 
    idx_t current_j = 0;

    EvidenceSetLocalState(ClientContext &context, const vector<LogicalType> &types) {
        chunk_i.Initialize(context, types);
        chunk_j.Initialize(context, types);
        diff_buffer.reserve(256);
        current_i = (idx_t)-1;
    }
};

// 1. FUNÇÃO DE BIND (Configura a estrutura da tabela de saída)
unique_ptr<FunctionData> EvidenceSetBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {

    auto table_name = input.inputs[0].GetValue<string>();

    // 1. Buscar a tabela para pegar os nomes das colunas
    auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);
    auto &entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, table_name);
    auto &table_entry = entry.Cast<TableCatalogEntry>();



    auto result = make_uniq<EvidenceSetBindData>();
    result->table_name = table_name;

    // Segundo parâmetro: Hashing
    if (input.inputs.size() > 1) {
        result->use_hashing = BooleanValue::Get(input.inputs[1]);
    }
    // Terceiro parâmetro: Vetorização
    if (input.inputs.size() > 2) {
        result->use_vectorization = BooleanValue::Get(input.inputs[2]);
    }


    // 2. Salvar nomes e tipos para o EvidenceSet
    for (auto &col : table_entry.GetColumns().Logical()) {
        result->table_column_names.push_back(col.GetName());
        result->table_types.push_back(col.GetType());
    }

    // Saída da função: apenas uma coluna com a lista de diferenças
    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("difference_set");

    return std::move(result);

}

// 2. FUNÇÃO DE INIT (Prepara o estado global)
unique_ptr<GlobalTableFunctionState> EvidenceSetInit(ClientContext &context, TableFunctionInitInput &input) {

    auto &bind_data = (EvidenceSetBindData &)*input.bind_data;
    
    // 1. Buscar a entrada no catálogo (usando o nome do esquema padrão 'main')
    auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);
    auto &entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, bind_data.table_name);
    auto &table_entry = entry.Cast<TableCatalogEntry>();
    idx_t actual_rows = table_entry.GetStorage().GetTotalRows();

    // Criar o estado com a coleção
    auto table_types = table_entry.GetTypes();
    auto state = make_uniq<EvidenceSetGlobalState>(actual_rows, context, table_types);

    // --- SCAN INTERNO ---
    auto &transaction = DuckTransaction::Get(context, catalog);
    auto &storage = table_entry.GetStorage();
    TableScanState scan_state;
    
    // Selecionar todas as colunas (índices 0 até N-1)

    vector<StorageIndex> column_ids; 
    for (idx_t i = 0; i < table_types.size(); i++) {
        // 2. O StorageIndex pode ser construído a partir do índice i
        column_ids.emplace_back(i);
    }


    storage.InitializeScan(context, transaction, scan_state, column_ids);

    DataChunk scan_chunk;
    scan_chunk.Initialize(context, table_types);

    while (true) {
        scan_chunk.Reset();
        // storage.Scan(context, scan_chunk, scan_state);
        storage.Scan(transaction, scan_chunk, scan_state);
        if (scan_chunk.size() == 0) break;
        state->collection->Append(scan_chunk);
    }

    fprintf(stderr, ">>> [DEBUG] Tabela carregada na memoria para comparacao.\n");
    fflush(stderr);

    return std::move(state);
}

unique_ptr<LocalTableFunctionState> EvidenceSetInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
    auto &bind_data = (EvidenceSetBindData &)*input.bind_data;
    return make_uniq<EvidenceSetLocalState>(context.client, bind_data.table_types);
}
// idx_t EvidenceSetMaxThreads(ClientContext &context, const FunctionData *bind_data_p) {
//     auto &bind_data = (EvidenceSetBindData &)*bind_data_p;
//     // Se a tabela for pequena, use 1 thread. Se grande, use o máximo disponível.
//     return 8; // Ou uma lógica baseada em bind_data.total_rows
// }

// 3. FUNÇÃO PRINCIPAL (Onde o loop acontece)
// void EvidenceSetFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
//     auto &state = (EvidenceSetGlobalState &)*data_p.global_state;
//     auto &bind_data = (EvidenceSetBindData &)*data_p.bind_data;
    
//     // LOG DE ENTRADA - Se isso não aparecer, o problema é antes da execução
//     fprintf(stderr, "\n>>> [DEBUG] Entrou na EvidenceSetFunction. i=%llu, j=%llu, total=%llu\n", 
//             (unsigned long long)state.row_i, (unsigned long long)state.row_j, (unsigned long long)state.total_rows);
//     fflush(stderr);
    
//     idx_t count = 0;
//     auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
//     auto num_cols = bind_data.table_column_names.size();

//     auto row_collection = state.collection->GetRows();

//     while (count < STANDARD_VECTOR_SIZE && state.row_i < state.total_rows - 1) {

//         string diff_set = "";
//         for (idx_t c = 0; c < num_cols; c++) {
//             // GetValue(coluna_index, linha_index)
//             auto val_i = row_collection.GetValue(c, state.row_i);
//             auto val_j = row_collection.GetValue(c, state.row_j);

//             // O operador != funciona nativamente para objetos Value
//             if (val_i != val_j) {
//                 if (!diff_set.empty()) {
//                     diff_set += ",";
//                 }
//                 diff_set += bind_data.table_column_names[c];
//             }
//         }

//         result_ptr[count] = StringVector::AddString(output.data[0], diff_set);
        
//         count++;
//         state.row_j++;
//         if (state.row_j >= state.total_rows) {
//             state.row_i++;
//             state.row_j = state.row_i + 1;
//         }

//         if (state.row_i >= state.total_rows - 1) {
//             fprintf(stderr, ">>> [DEBUG] Loop concluído dentro do chunk. Total gerado: %llu\n", (unsigned long long)count);
//             break;
//         }
//     }
//     output.SetCardinality(count);
// }


// void EvidenceSetFunctionParallelOld(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
//     auto &gstate = (EvidenceSetGlobalState &)*data_p.global_state;
//     auto &lstate = (EvidenceSetLocalState &)*data_p.local_state;
//     auto &bind_data = (EvidenceSetBindData &)*data_p.bind_data;

//     idx_t count = 0;
//     auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
//     auto num_cols = bind_data.table_column_names.size();

//     auto row_collection = gstate.collection->GetRows();

//     // Log para provar paralelismo:
//     auto thread_id = std::this_thread::get_id();
//     fprintf(stderr, ">>> [THREAD %zu] Processando linha i=%llu\n", 
//             std::hash<std::thread::id>{}(thread_id), (unsigned long long)gstate.current_row_i);

//     // Cada thread pega uma linha 'i' para comparar com o resto
//     while (count < STANDARD_VECTOR_SIZE) {
//         idx_t i = gstate.current_row_i;
//         if (i >= gstate.total_rows - 1) break;


//         for (idx_t j = i + 1; j < gstate.total_rows; j++) {
//             if (count >= STANDARD_VECTOR_SIZE) {
//                 // Se o chunk de saída encher, precisamos salvar o estado e voltar
//                 // Nota: Para simplificar no mestrado, i++ ocorre no loop externo
//                 break;
//             }



//             // Filtro de Hash (Opcional)
//             if (bind_data.use_hashing && gstate.row_hashes[i] == gstate.row_hashes[j]) {
//                 result_ptr[count++] = StringVector::AddString(output.data[0], "");
//                 continue;
//             }



//             lstate.diff_buffer.clear();


//             if (bind_data.use_vectorization) {
//                 // --- CAMINHO VETORIZADO (ACESSA ROW EM CACHE) ---
//                 auto row_i = row_collection[i];
//                 auto row_j = row_collection[j];
//                 // auto row_j = row_collection.GetRow(j);
//                 for (idx_t c = 0; c < num_cols; c++) {
//                     if (row_i.GetValue(c) != row_j.GetValue(c)) {
//                         if (!lstate.diff_buffer.empty()) lstate.diff_buffer += ",";
//                         lstate.diff_buffer += bind_data.table_column_names[c];
//                     }
//                 }
//             } else {
//                 // --- CAMINHO ESCALAR (ANTERIOR - ACESSA COLLECTION DIRETO) ---
//                 for (idx_t c = 0; c < num_cols; c++) {
//                     auto val_i = gstate.collection->GetRows().GetValue(c, i);
//                     auto val_j = gstate.collection->GetRows().GetValue(c, j);
//                     if (val_i != val_j) {
//                         if (!lstate.diff_buffer.empty()) lstate.diff_buffer += ",";
//                         lstate.diff_buffer += bind_data.table_column_names[c];
//                     }
//                 }
//             }

//             result_ptr[count++] = StringVector::AddString(output.data[0], lstate.diff_buffer);
//         }
//     }
//     output.SetCardinality(count);
// }

void EvidenceSetFunctionParallel(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &gstate = (EvidenceSetGlobalState &)*data_p.global_state;
    auto &lstate = (EvidenceSetLocalState &)*data_p.local_state;
    auto &bind_data = (EvidenceSetBindData &)*data_p.bind_data;

    idx_t count = 0;
    auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
    auto num_cols = bind_data.table_column_names.size();
    auto row_collection = gstate.collection->GetRows();

    while (count < STANDARD_VECTOR_SIZE) {
        // 1. Controle de Lote: Se não tem trabalho, pega um lote pequeno
        if (lstate.current_i == (idx_t)-1 || (lstate.current_i >= lstate.i_end && lstate.current_j >= gstate.total_rows)) {
            // Pegamos apenas 5 linhas 'i' por vez para forçar as threads a revezarem mais rápido
            lstate.i_start = gstate.current_row_i.fetch_add(10); 
            lstate.i_end = MinValue(lstate.i_start + 10, gstate.total_rows);
            lstate.current_i = lstate.i_start;
            lstate.current_j = lstate.current_i + 1;
        }

        if (lstate.current_i >= gstate.total_rows || lstate.current_i >= lstate.i_end) break;
        
        idx_t i = lstate.current_i;

        // 2. Loop J
        for (; lstate.current_j < gstate.total_rows && count < STANDARD_VECTOR_SIZE; lstate.current_j++) {
            idx_t j = lstate.current_j;

            // Log de Debug: Veremos IDs diferentes agora!
            if (j % 5000 == 0) {
                auto thread_id = std::this_thread::get_id();
                fprintf(stderr, ">>> [THREAD %zu] i=%llu, j=%llu\n", 
                    std::hash<std::thread::id>{}(thread_id), (unsigned long long)i, (unsigned long long)j);
            }

            // --- Lógica de Comparação ---
            if (bind_data.use_hashing && gstate.row_hashes[i] == gstate.row_hashes[j]) {
                result_ptr[count++] = StringVector::AddString(output.data[0], "");
                continue;
            }

            lstate.diff_buffer.clear();
            if (bind_data.use_vectorization) {
                auto row_i = row_collection[i];
                auto row_j = row_collection[j];
                for (idx_t c = 0; c < num_cols; c++) {
                    if (row_i.GetValue(c) != row_j.GetValue(c)) {
                        if (!lstate.diff_buffer.empty()) lstate.diff_buffer += ",";
                        lstate.diff_buffer += bind_data.table_column_names[c];
                    }
                }
            } else {
                for (idx_t c = 0; c < num_cols; c++) {
                    if (row_collection.GetValue(c, i) != row_collection.GetValue(c, j)) {
                        if (!lstate.diff_buffer.empty()) lstate.diff_buffer += ",";
                        lstate.diff_buffer += bind_data.table_column_names[c];
                    }
                }
            }

            // REMOVIDO o sleep_for longo para o paralelismo fluir melhor
            result_ptr[count++] = StringVector::AddString(output.data[0], lstate.diff_buffer);
        }

        // 3. Avanço: Se terminou o J, vai para o próximo I do lote local
        if (lstate.current_j >= gstate.total_rows) {
            lstate.current_i++;
            lstate.current_j = lstate.current_i + 1;
        } else {
            // Se saiu porque o buffer de 2048 encheu, para e volta depois
            break;
        }
    }
    output.SetCardinality(count);
}
// 1. Função de estimativa de linhas
unique_ptr<NodeStatistics> EvidenceSetCardinality(ClientContext &context, const FunctionData *bind_data_p) {
    auto &bind_data = (EvidenceSetBindData &)*bind_data_p;
    // Se a tabela tem N linhas, o Evidence Set tem (N*(N-1))/2 linhas.
    // Vamos apenas retornar um número grande para "assustar" o otimizador.
    return make_uniq<NodeStatistics>(100000000); 
}

// Registro
void RegisterEvidenceSet(ExtensionLoader &loader) {
    // 1. Definir a assinatura da função
    // Nome, argumentos de entrada (vazio por enquanto), função principal, bind e init
    TableFunction evidence_set_fun("get_evidence_set", 
                                    {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BOOLEAN}, 
                                    EvidenceSetFunctionParallel, 
                                    EvidenceSetBind, 
                                    EvidenceSetInit);

    evidence_set_fun.init_local = EvidenceSetInitLocal;
    
    evidence_set_fun.cardinality = EvidenceSetCardinality;

    // ESSA LINHA É O QUE ATIVA O SCHEDULER:
    evidence_set_fun.projection_pushdown = true; 
    evidence_set_fun.filter_pushdown = true;

    // 2. Registrar usando o loader (mesma forma que você registrou a 'sumsq')
    loader.RegisterFunction(evidence_set_fun);
}


// LIMA DISCOVER

struct LimaDiscoverBindData : public TableFunctionData {
    string table_name;
    LimaSchema schema;
};

static bool EvaluatePredicate(const LimaPredicate &pred, const Value &v1, const Value &v2) {
    if (v1.IsNull() || v2.IsNull()) return false;
    switch (pred.op) {
        case LimaOperator::EQ: return v1 == v2;
        case LimaOperator::NE: return v1 != v2;
        case LimaOperator::GT: return v1 > v2;
        case LimaOperator::GE: return v1 >= v2;
        case LimaOperator::LT: return v1 < v2;
        case LimaOperator::LE: return v1 <= v2;
        default: return false;
    }
}

struct LimaDiscoverGlobalState : public GlobalTableFunctionState {
    shared_ptr<ColumnDataCollection> collection;
    bool finished = false;

    // Lattice: Root is empty conjunction
    unordered_map<LimaNode, unique_ptr<LimaNode>, LimaNodeHash> nodes;
    LimaNode* root;

    // Sampling state
    idx_t total_rows = 0;
    std::mt19937 rng;
    
    // Predicate results: [predicate_id][word_idx]
    vector<vector<uint64_t>> predicate_results;
    idx_t num_samples = 0;
    idx_t num_words = 0;

    // Discovered constraints
    vector<string> discovered_constraints;
    idx_t result_idx = 0;

    LimaDiscoverGlobalState(ClientContext &context, const vector<LogicalType> &types, idx_t num_cols) 
        : rng(42) { // Fixed seed for reproducibility
        collection = make_shared_ptr<ColumnDataCollection>(context, types);
        auto empty_root = make_uniq<LimaNode>(num_cols);
        empty_root->CalculateHash();
        root = empty_root.get();
        nodes[(*empty_root)] = std::move(empty_root);
    }

    LimaNode* GetOrCreateNode(const LimaNode& n) {
        auto it = nodes.find(n);
        if (it != nodes.end()) return it->second.get();
        auto new_node = make_uniq<LimaNode>(n);
        new_node->CalculateHash();
        auto* ptr = new_node.get();
        nodes[n] = std::move(new_node);
        return ptr;
    }

    void SetPredicateResult(idx_t pred_idx, idx_t pair_idx, bool value) {
        if (value) {
            predicate_results[pred_idx][pair_idx / 64] |= (1ULL << (pair_idx % 64));
        }
    }

    void AddSamples(idx_t n, const LimaSchema& schema) {
        idx_t start_idx = num_samples;
        num_samples += n;
        num_words = (num_samples + 63) / 64;

        for (auto &res : predicate_results) {
            res.resize(num_words, 0);
        }

        std::uniform_int_distribution<idx_t> dist(0, total_rows - 1);
        auto row_collection = collection->GetRows();

        for (idx_t i = start_idx; i < num_samples; i++) {
            idx_t r1 = dist(rng);
            idx_t r2 = dist(rng);
            while (r1 == r2) r2 = dist(rng);

            for (idx_t p_idx = 0; p_idx < schema.all_predicates.size(); p_idx++) {
                auto &pred = schema.all_predicates[p_idx];
                auto v1 = row_collection.GetValue(pred.col_idx, r1);
                auto v2 = row_collection.GetValue(pred.col_idx, r2);
                if (EvaluatePredicate(pred, v1, v2)) {
                    SetPredicateResult(p_idx, i, true);
                }
            }
        }
    }
};

unique_ptr<FunctionData> LimaDiscoverBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
    auto table_name = input.inputs[0].GetValue<string>();
    auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);
    auto &entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, table_name);
    auto &table_entry = entry.Cast<TableCatalogEntry>();

    auto result = make_uniq<LimaDiscoverBindData>();
    result->table_name = table_name;

    vector<string> col_names;
    vector<LogicalType> col_types;
    for (auto &col : table_entry.GetColumns().Logical()) {
        col_names.push_back(col.GetName());
        col_types.push_back(col.GetType());
    }

    result->schema.Initialize(col_names, col_types);
    // result->schema.Print(stderr); // Clean up stderr for next test

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("denial_constraint");

    return std::move(result);
}

unique_ptr<GlobalTableFunctionState> LimaDiscoverInit(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind_data = (LimaDiscoverBindData &)*input.bind_data;
    auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);
    auto &entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, bind_data.table_name);
    auto &table_entry = entry.Cast<TableCatalogEntry>();

    auto table_types = table_entry.GetTypes();
    auto state = make_uniq<LimaDiscoverGlobalState>(context, table_types, table_types.size());
    state->total_rows = table_entry.GetStorage().GetTotalRows();

    auto &transaction = DuckTransaction::Get(context, catalog);
    auto &storage = table_entry.GetStorage();
    TableScanState scan_state;
    vector<StorageIndex> column_ids;
    for (idx_t i = 0; i < table_types.size(); i++) {
        column_ids.emplace_back(i);
    }

    storage.InitializeScan(context, transaction, scan_state, column_ids);
    DataChunk scan_chunk;
    scan_chunk.Initialize(context, table_types);

    while (true) {
        scan_chunk.Reset();
        storage.Scan(transaction, scan_chunk, scan_state);
        if (scan_chunk.size() == 0) break;
        state->collection->Append(scan_chunk);
    }

    fprintf(stderr, ">>> [LIMA] Table %s loaded. Total rows: %llu\n", bind_data.table_name.c_str(), 
            (unsigned long long)state->collection->Count());
    fflush(stderr);

    return std::move(state);
}

static vector<uint64_t> GetNodeBitset(LimaDiscoverGlobalState &state, LimaNode &node, const LimaSchema& schema) {
    vector<uint64_t> res(state.num_words, ~0ULL); 
    bool empty = true;
    for (size_t i = 0; i < node.predicate_ids.size(); i++) {
        if (node.predicate_ids[i] != -1) {
            empty = false;
            idx_t p_idx = 0;
            bool found = false;
            for(auto &p : schema.all_predicates) {
                if (p.col_idx == i && (int8_t)p.op == node.predicate_ids[i]) {
                    found = true;
                    break;
                }
                p_idx++;
            }
            if (found) {
                for (idx_t w = 0; w < state.num_words; w++) {
                    res[w] &= state.predicate_results[p_idx][w];
                }
            }
        }
    }
    if (empty) return vector<uint64_t>(state.num_words, 0);
    return res;
}

void PropagateAcross(LimaDiscoverGlobalState &state, LimaNode* from, LimaNode* to, idx_t col_idx, const LimaSchema& schema) {
    auto from_bits = GetNodeBitset(state, *from, schema);
    auto to_bits = GetNodeBitset(state, *to, schema);

    idx_t a = 0;
    for (auto w : to_bits) a += __builtin_popcountll(w);
    idx_t total_samples = state.num_samples;
    idx_t b = total_samples - a;

    to->dist.a = 1 + a;
    to->dist.b = 1 + b;
    to->dist.updateStats();

    idx_t from_a = 0;
    bool root_from = true;
    for(auto pid : from->predicate_ids) if(pid != -1) root_from = false;

    if (root_from) {
        from_a = total_samples;
    } else {
        for (auto w : from_bits) from_a += __builtin_popcountll(w);
    }

    to->edge_dist.a = 1 + a;
    to->edge_dist.b = 1 + (from_a - a);
    to->edge_dist.updateStats();

    to->sound = true;
    double min_norm_dist = 1e10;

    for (size_t i = 0; i < to->predicate_ids.size(); i++) {
        if (to->predicate_ids[i] != -1 && i != col_idx) {
            LimaNode parent_node_struct = *to;
            parent_node_struct.predicate_ids[i] = -1;
            parent_node_struct.CalculateHash();
            LimaNode* parent = state.GetOrCreateNode(parent_node_struct);

            BetaDistribution parent_to_to_edge_dist;
            auto parent_bits = GetNodeBitset(state, *parent, schema);
            idx_t pa = 0;
            for (auto w : parent_bits) pa += __builtin_popcountll(w);
            
            parent_to_to_edge_dist.a = 1 + a;
            parent_to_to_edge_dist.b = 1 + (pa - a);
            parent_to_to_edge_dist.updateStats();

            auto &lower = parent_to_to_edge_dist;
            auto &upper = to->edge_dist;

            double combined_sd = std::sqrt(lower.sdLogOdds * lower.sdLogOdds + upper.sdLogOdds * upper.sdLogOdds);
            double norm_dist = (lower.meanLogOdds - upper.meanLogOdds) / combined_sd;

            if (norm_dist < min_norm_dist) min_norm_dist = norm_dist;

            if (norm_dist < 2.0) { 
                to->sound = false;
                break;
            }
        }
    }

    if (to->sound && a == 0) {
        state.discovered_constraints.push_back(to->ToString(schema));
    }
}

void Search(LimaDiscoverGlobalState &state, LimaNode* node, idx_t last_col_idx, const LimaSchema& schema) {
    if (node->dist.a == 1 && node != state.root) return;

    for (idx_t i = 0; i < last_col_idx; i++) {
        if (node->predicate_ids[i] != -1) continue;

        for (auto &pred : schema.columns[i].predicates) {
            LimaNode single_pred_node(schema.columns.size());
            single_pred_node.predicate_ids[i] = (int8_t)pred.op;
            single_pred_node.CalculateHash();
            if (state.nodes.count(single_pred_node) == 0 || state.nodes[single_pred_node]->dist.grad < 1e-6) continue;

            LimaNode next_node_struct = *node;
            next_node_struct.predicate_ids[i] = (int8_t)pred.op;
            next_node_struct.CalculateHash();
            
            LimaNode* next_node = state.GetOrCreateNode(next_node_struct);
            PropagateAcross(state, node, next_node, i, schema);
            
            if (next_node->sound) {
                Search(state, next_node, i, schema);
            }
        }
    }
}

void LimaDiscoverFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = (LimaDiscoverGlobalState &)*data_p.global_state;
    auto &bind_data = (LimaDiscoverBindData &)*data_p.bind_data;
    
    if (state.result_idx < state.discovered_constraints.size()) {
        idx_t count = 0;
        auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
        while (count < STANDARD_VECTOR_SIZE && state.result_idx < state.discovered_constraints.size()) {
            result_ptr[count++] = StringVector::AddString(output.data[0], state.discovered_constraints[state.result_idx++]);
        }
        output.SetCardinality(count);
        return;
    }

    if (state.finished) {
        output.SetCardinality(0);
        return;
    }

    if (state.total_rows < 2) {
        state.finished = true;
        output.SetCardinality(0);
        return;
    }

    state.predicate_results.resize(bind_data.schema.all_predicates.size());
    idx_t batches = 5; 
    for (idx_t b = 0; b < batches; b++) {
        idx_t batch_size = 100 * (1 << b);
        state.AddSamples(batch_size, bind_data.schema);

        for (idx_t p_idx = 0; p_idx < bind_data.schema.all_predicates.size(); p_idx++) {
            auto &pred = bind_data.schema.all_predicates[p_idx];
            LimaNode n(bind_data.schema.columns.size());
            n.predicate_ids[pred.col_idx] = (int8_t)pred.op;
            n.CalculateHash();
            auto* node = state.GetOrCreateNode(n);

            idx_t a = 0;
            for (idx_t w = 0; w < state.num_words; w++) {
                a += __builtin_popcountll(state.predicate_results[p_idx][w]);
            }
            node->dist.a = 1 + a;
            node->dist.b = 1 + (state.num_samples - a);
            node->dist.updateStats();
        }
    }

    fprintf(stderr, ">>> [LIMA] Starting Lattice Search...\n");
    Search(state, state.root, bind_data.schema.columns.size(), bind_data.schema);
    fprintf(stderr, ">>> [LIMA] Search complete. Found %llu constraints.\n", (unsigned long long)state.discovered_constraints.size());

    state.finished = true;
    if (state.discovered_constraints.empty()) {
        output.SetCardinality(0);
    } else {
        idx_t count = 0;
        auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
        while (count < STANDARD_VECTOR_SIZE && state.result_idx < state.discovered_constraints.size()) {
            result_ptr[count++] = StringVector::AddString(output.data[0], state.discovered_constraints[state.result_idx++]);
        }
        output.SetCardinality(count);
    }
}

// Registro
void RegisterLimaDiscover(ExtensionLoader &loader) {
    TableFunction lima_discover_fun("lima_discover", {LogicalType::VARCHAR}, LimaDiscoverFunction, LimaDiscoverBind, LimaDiscoverInit);
    loader.RegisterFunction(lima_discover_fun);
}


static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto duckteste_scalar_function = ScalarFunction("duckteste", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DucktesteScalarFun);
	loader.RegisterFunction(duckteste_scalar_function);

	auto duckteste_binary_function = ScalarFunction("duckteste_binary", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                                 LogicalType::VARCHAR, DucktesteBinaryFun);
	loader.RegisterFunction(duckteste_binary_function);

	// Register another scalar function
	auto duckteste_openssl_version_scalar_function = ScalarFunction("duckteste_openssl_version", {LogicalType::VARCHAR},
	                                                            LogicalType::VARCHAR, DucktesteOpenSSLVersionScalarFun);
	loader.RegisterFunction(duckteste_openssl_version_scalar_function);

	AggregateFunctionSet sumsq("sumsq");
    RegisterSumSquares(sumsq);
    loader.RegisterFunction(sumsq);

    RegisterEvidenceSet(loader);
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
