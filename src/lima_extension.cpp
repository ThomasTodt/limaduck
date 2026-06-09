#define DUCKDB_EXTENSION_MAIN

#include "lima_extension.hpp"
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

// --- 1. Estado Global ---
struct LimaDiscoverGlobalState : public GlobalTableFunctionState {
	// Usando o nosso CSVDataset temporariamente para teste
	std::unique_ptr<DuckDBDataset> dataset;
	std::unique_ptr<Scheduler> scheduler;

	std::vector<std::string> discovered_constraints;
	idx_t result_idx = 0;
	bool finished_search = false;
};

// --- 2. BIND (Captura os argumentos do SQL) ---
unique_ptr<FunctionData> LimaDiscoverBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<LimaDiscoverBindData>();

	// 1. Argumento Obrigatório: Nome do Arquivo (Sempre o índice 0)
	result->file_name = input.inputs[0].GetValue<string>();

	// 2. Valores Padrão
	result->num_lines = -1;        // -1 será o nosso código secreto para "Leia o arquivo inteiro"
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

// --- 3. INIT (Injeta as variáveis no motor) ---
unique_ptr<GlobalTableFunctionState> LimaDiscoverInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<LimaDiscoverGlobalState>();
	auto &bind_data = (LimaDiscoverBindData &)*input.bind_data;

	double real_threshold = bind_data.threshold * 0.01;

	state->dataset = make_uniq<DuckDBDataset>(context, bind_data.file_name, bind_data.num_lines);

	state->scheduler = make_uniq<Scheduler>(state->dataset.get(), real_threshold);

	return std::move(state);
}

// --- 4. EXECUÇÃO ---
void LimaDiscoverFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = (LimaDiscoverGlobalState &)*data_p.global_state;

	if (!state.finished_search) {
		state.scheduler->populatePredicates(state.discovered_constraints);

		state.finished_search = true;
		fprintf(stderr, ">>> [LIMA] Busca completa. Encontradas %zu restricoes.\n",
		        state.discovered_constraints.size());
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

// --- 5. REGISTRO (Avisa o DuckDB sobre os argumentos) ---
void RegisterLimaDiscover(ExtensionLoader &loader) {
	// Registra a função com 1 argumento posicional (VARCHAR para o nome do arquivo)
	TableFunction lima_discover_fun("lima_discover", {LogicalType::VARCHAR}, LimaDiscoverFunction, LimaDiscoverBind,
	                                LimaDiscoverInit);

	// Registra os parâmetros nomeados opcionais
	lima_discover_fun.named_parameters["num_lines"] = LogicalType::INTEGER;
	lima_discover_fun.named_parameters["threshold"] = LogicalType::DOUBLE;

	loader.RegisterFunction(lima_discover_fun);
}

static void LoadInternal(ExtensionLoader &loader) {
	RegisterLimaDiscover(loader);
}
void LimaExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string LimaExtension::Name() {
	return "lima";
}
std::string LimaExtension::Version() const {
	return "";
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(lima, loader) {
	duckdb::LoadInternal(loader);
}
}
