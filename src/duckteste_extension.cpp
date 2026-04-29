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