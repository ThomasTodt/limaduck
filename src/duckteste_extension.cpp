#define DUCKDB_EXTENSION_MAIN

#include "duckteste_extension.hpp"
#include "duckdb.hpp"
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
};

struct EvidenceSetLocalState : public LocalTableFunctionState {
    DataChunk chunk_i;
    DataChunk chunk_j;
    // Buffer para evitar realocações de string constantes
    string diff_buffer;

    EvidenceSetLocalState(ClientContext &context, const vector<LogicalType> &types) {
        chunk_i.Initialize(context, types);
        chunk_j.Initialize(context, types);
        diff_buffer.reserve(256); // pq 256??
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

void EvidenceSetFunctionParallel(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &gstate = (EvidenceSetGlobalState &)*data_p.global_state;
    auto &lstate = (EvidenceSetLocalState &)*data_p.local_state;
    auto &bind_data = (EvidenceSetBindData &)*data_p.bind_data;

    idx_t count = 0;
    auto result_ptr = FlatVector::GetData<string_t>(output.data[0]);
    auto num_cols = bind_data.table_column_names.size();

    auto row_collection = gstate.collection->GetRows();

    // Log para provar paralelismo:
    auto thread_id = std::this_thread::get_id();
    fprintf(stderr, ">>> [THREAD %zu] Processando linha i=%llu\n", 
            std::hash<std::thread::id>{}(thread_id), (unsigned long long)gstate.current_row_i);

    // Cada thread pega uma linha 'i' para comparar com o resto
    while (count < STANDARD_VECTOR_SIZE) {
        idx_t i = gstate.current_row_i++;
        if (i >= gstate.total_rows - 1) break;


        for (idx_t j = i + 1; j < gstate.total_rows; j++) {
            if (count >= STANDARD_VECTOR_SIZE) {
                // Se o chunk de saída encher, precisamos salvar o estado e voltar
                // Nota: Para simplificar no mestrado, i++ ocorre no loop externo
                break;
            }



            // Filtro de Hash (Opcional)
            if (bind_data.use_hashing && gstate.row_hashes[i] == gstate.row_hashes[j]) {
                result_ptr[count++] = StringVector::AddString(output.data[0], "");
                continue;
            }



            lstate.diff_buffer.clear();


            if (bind_data.use_vectorization) {
                // --- CAMINHO VETORIZADO (ACESSA ROW EM CACHE) ---
                auto row_i = row_collection[i];
                auto row_j = row_collection[j];
                // auto row_j = row_collection.GetRow(j);
                for (idx_t c = 0; c < num_cols; c++) {
                    if (row_i.GetValue(c) != row_j.GetValue(c)) {
                        if (!lstate.diff_buffer.empty()) lstate.diff_buffer += ",";
                        lstate.diff_buffer += bind_data.table_column_names[c];
                    }
                }
            } else {
                // --- CAMINHO ESCALAR (ANTERIOR - ACESSA COLLECTION DIRETO) ---
                for (idx_t c = 0; c < num_cols; c++) {
                    auto val_i = gstate.collection->GetRows().GetValue(c, i);
                    auto val_j = gstate.collection->GetRows().GetValue(c, j);
                    if (val_i != val_j) {
                        if (!lstate.diff_buffer.empty()) lstate.diff_buffer += ",";
                        lstate.diff_buffer += bind_data.table_column_names[c];
                    }
                }
            }

            result_ptr[count++] = StringVector::AddString(output.data[0], lstate.diff_buffer);
        }
    }
    output.SetCardinality(count);
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
    
    // ESSA LINHA É O QUE ATIVA O SCHEDULER:
    evidence_set_fun.projection_pushdown = true; 
    evidence_set_fun.filter_pushdown = true;

    // 2. Registrar usando o loader (mesma forma que você registrou a 'sumsq')
    loader.RegisterFunction(evidence_set_fun);
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
