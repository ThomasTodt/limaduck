#include "DuckDBDataset.hpp"
#include "Schema.hpp"
#include "TPSubSet.hpp"
#include "TPSet.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/common/helper.hpp"
#include <iostream>
#include <cassert>

namespace duckdb {

static Schema *BuildDuckSchema(ClientContext &context, std::string &table_name) {
	auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);

	if (!catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, table_name,
	                      OnEntryNotFound::RETURN_NULL)) {
		// Se não existe como tabela, tenta carregar como CSV
		std::string temp_name = "lima_auto_csv_" + std::to_string((uint64_t)&context);
		auto query =
		    "CREATE OR REPLACE TEMP TABLE " + temp_name + " AS SELECT * FROM read_csv_auto('" + table_name + "')";
		auto res = context.Query(query, false);
		if (!res->HasError()) {
			table_name = temp_name;
		}
	}

	auto &entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, table_name);
	auto &table_entry = entry.Cast<TableCatalogEntry>();
	// std::cerr << "[RADAR] 4. Cast para TableCatalogEntry realizado.\n";

	std::vector<std::string> colnames;
	std::vector<Column::Type> coltypes; // NOVO: Vetor de Tipos

	for (auto &col : table_entry.GetColumns().Logical()) {
		colnames.push_back(col.GetName());
		// std::cerr << "[RADAR] 4.1. Coluna encontrada: " << col.GetName() << "\n";

		// TRADUTOR DE TIPOS: DuckDB -> LIMA
		switch (col.GetType().id()) {
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
			coltypes.push_back(Column::Type::INTEGER);
			break;
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::DECIMAL:
			coltypes.push_back(Column::Type::REAL);
			break;
		default:
			// VARCHAR, DATE, BOOLEAN, etc, viram STRING para a lógica de EQ/NE do LIMA
			coltypes.push_back(Column::Type::STRING);
			break;
		}
	}
	// std::cerr << "[RADAR] 5. Vetor de colunas preenchido. Total de colunas: " << colnames.size() << "\n";

	// std::cerr << "[RADAR] 6. Chamando construtor 'new Schema(colnames)'...\n";
	Schema *s = new Schema(colnames, coltypes);

	// std::cerr << "[RADAR] 7. Schema instanciado! Retornando para construtor pai (RelationalDataset)...\n";
	return s;
}

DuckDBDataset::DuckDBDataset(ClientContext &context, std::string table_name, int32_t num_lines)
    : RelationalDataset(BuildDuckSchema(context, table_name), 0) {

	// std::cerr << "[RADAR] 8. Entrou no construtor do DuckDBDataset! (O pai ja foi inicializado)\n";
	random_engine.seed(42);
	// std::cerr << ">>> [LIMA] Conectando diretamente na memoria do DuckDB: " << table_name << "\n";

	auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);
	auto &entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, table_name);
	auto &table_entry = entry.Cast<TableCatalogEntry>();

	int64_t total_rows = table_entry.GetStorage().GetTotalRows();
	this->size = (num_lines > 0 && num_lines < total_rows) ? num_lines : total_rows;
	int32_t n = this->size;
	// std::cerr << ">>> [LIMA] Tabela encontrada! Linhas: " << n << " | Colunas: " << schema->columns.size() << "\n";

	this->constColumn.assign(schema->columns.size(), true);

	// --- 1. ALOCAR ESPAÇO PARA OS DADOS (Restaurando sua arquitetura) ---
	size_t numInt = schema->typeColumns[Column::Type::INTEGER].size();
	intData.assign(numInt, std::vector<int32_t>(n));

	size_t numReal = schema->typeColumns[Column::Type::REAL].size();
	realData.assign(numReal, std::vector<float>(n));

	size_t numStr = schema->typeColumns[Column::Type::STRING].size();
	stringData.assign(numStr, std::vector<int32_t>(n));

	auto &intCols = schema->typeColumns[Column::Type::INTEGER];
	auto &realCols = schema->typeColumns[Column::Type::REAL];
	auto &strCols = schema->typeColumns[Column::Type::STRING];

	// --- 2. O SCANNER COLUNAR DO DUCKDB ---
	auto &transaction = DuckTransaction::Get(context, catalog);
	auto &storage = table_entry.GetStorage();
	TableScanState scan_state;

	auto table_types = table_entry.GetTypes();
	vector<StorageIndex> column_ids;
	for (idx_t i = 0; i < table_types.size(); i++)
		column_ids.emplace_back(i);

	storage.InitializeScan(context, transaction, scan_state, column_ids);

	DataChunk scan_chunk;
	scan_chunk.Initialize(context, table_types);

	idx_t current_row = 0;
	while (true) {
		scan_chunk.Reset();
		storage.Scan(transaction, scan_chunk, scan_state);

		idx_t chunk_size = scan_chunk.size();

		if (chunk_size == 0)
			break;

		if (current_row + chunk_size > n) {
			chunk_size = n - current_row;
			;
		}

		for (size_t i = 0; i < intCols.size(); i++) {
			int32_t colID = intCols[i]->ID;
			for (idx_t r = 0; r < chunk_size; r++) {
				auto val = scan_chunk.GetValue(colID, r);
				// GetValue<int32_t>() evita a alocação de Strings e o std::stoi
				intData[i][current_row + r] = val.IsNull() ? 0 : val.GetValue<int32_t>();
				if (constColumn[colID] && intData[i][current_row + r] != intData[i][0])
					constColumn[colID] = false;
			}
		}

		for (size_t i = 0; i < realCols.size(); i++) {
			int32_t colID = realCols[i]->ID;
			for (idx_t r = 0; r < chunk_size; r++) {
				auto val = scan_chunk.GetValue(colID, r);
				realData[i][current_row + r] =
				    val.IsNull() ? std::numeric_limits<float>::quiet_NaN() : val.GetValue<float>();
				if (constColumn[colID] && realData[i][current_row + r] != realData[i][0])
					constColumn[colID] = false;
			}
		}

		for (size_t i = 0; i < strCols.size(); i++) {
			int32_t colID = strCols[i]->ID;
			for (idx_t r = 0; r < chunk_size; r++) {
				auto val = scan_chunk.GetValue(colID, r);
				// GetValue<std::string>() é infinitamente mais rápido que ToString()
				std::string k = val.IsNull() ? "" : val.GetValue<std::string>();

				auto it = stringMap.find(k);
				int32_t v;
				if (it == stringMap.end()) {
					v = stringMap.size() + 1;
					stringMap[k] = v;
					invStringMap.push_back(k);
				} else {
					v = it->second;
				}
				stringData[i][current_row + r] = v;
				if (constColumn[colID] && stringData[i][current_row + r] != stringData[i][0])
					constColumn[colID] = false;
			}
		}

		current_row += chunk_size;
		if (current_row >= n)
			break;

		// for (idx_t r = 0; r < scan_chunk.size(); r++) {
		//     if (current_row >= n) break;
		//     // --- INTEIROS ---
		//     for (size_t i = 0; i < intCols.size(); i++) {
		//         int32_t colID = intCols[i]->ID;
		//         auto val = scan_chunk.GetValue(colID, r);
		//         intData[i][current_row] = val.IsNull() ? 0 : std::stoi(val.ToString());
		//         if (constColumn[colID] && intData[i][current_row] != intData[i][0]) constColumn[colID] = false;
		//     }

		//     // --- REAIS (FLOAT) ---
		//     for (size_t i = 0; i < realCols.size(); i++) {
		//         int32_t colID = realCols[i]->ID;
		//         auto val = scan_chunk.GetValue(colID, r);
		//         realData[i][current_row] = val.IsNull() ? std::numeric_limits<float>::quiet_NaN() :
		//         std::stof(val.ToString()); if (constColumn[colID] && realData[i][current_row] != realData[i][0])
		//         constColumn[colID] = false;
		//     }

		//     // --- STRINGS ---
		//     for (size_t i = 0; i < strCols.size(); i++) {
		//         int32_t colID = strCols[i]->ID;
		//         auto val = scan_chunk.GetValue(colID, r);
		//         std::string k = val.IsNull() ? "" : val.ToString();

		//         auto it = stringMap.find(k);
		//         int32_t v;
		//         if (it == stringMap.end()) {
		//             v = stringMap.size() + 1;
		//             stringMap[k] = v;
		//             invStringMap.push_back(k);
		//         } else {
		//             v = it->second;
		//         }
		//         stringData[i][current_row] = v;
		//         if (constColumn[colID] && stringData[i][current_row] != stringData[i][0]) constColumn[colID] = false;
		//     }
		//     current_row++;
		// }
		// if (current_row >= n) break;
	}
	// std::cerr << ">>> [LIMA] Ingestao de RAM completa (" << current_row << " linhas).\n";
}

// --- 3. O SEU FILTRO PERFEITO INTACTO ---
std::unique_ptr<TPSubSet> DuckDBDataset::filter(TPSubSet &TPs, Predicate *pred) {
	auto newTPSubset = make_uniq<TPSubSet>(TPs.source);

	const auto &tpx = TPs.source->x;
	const auto &tpy = TPs.source->y;
	const auto &oldTPs = TPs.TPs;
	auto &newTPs = newTPSubset->TPs;

	int32_t newLength = 0;

	switch (pred->cols->type) {
	case Column::Type::STRING: {
		const auto &sdx = stringData[pred->cols->x->typeID];
		const auto &sdy = stringData[pred->cols->y->typeID];
		switch (pred->op) {
		case Predicate::Operator::EQ:
			for (int32_t tp : oldTPs) {
				if (sdx[tpx[tp]] == sdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::NE:
			for (int32_t tp : oldTPs) {
				if (sdx[tpx[tp]] != sdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		default:
			assert(false && "Operador invalido para STRING");
		}
		break;
	}
	case Column::Type::INTEGER: {
		const auto &idx = intData[pred->cols->x->typeID];
		const auto &idy = intData[pred->cols->y->typeID];
		switch (pred->op) {
		case Predicate::Operator::EQ:
			for (int32_t tp : oldTPs) {
				if (idx[tpx[tp]] == idy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::NE:
			for (int32_t tp : oldTPs) {
				if (idx[tpx[tp]] != idy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::GT:
			for (int32_t tp : oldTPs) {
				if (idx[tpx[tp]] > idy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::LE:
			for (int32_t tp : oldTPs) {
				if (idx[tpx[tp]] <= idy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::LT:
			for (int32_t tp : oldTPs) {
				if (idx[tpx[tp]] < idy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::GE:
			for (int32_t tp : oldTPs) {
				if (idx[tpx[tp]] >= idy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		default:
			assert(false && "Operador invalido para INTEGER");
		}
		break;
	}
	case Column::Type::REAL: {
		const auto &rdx = realData[pred->cols->x->typeID];
		const auto &rdy = realData[pred->cols->y->typeID];
		switch (pred->op) {
		case Predicate::Operator::EQ:
			for (int32_t tp : oldTPs) {
				if (rdx[tpx[tp]] == rdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::NE:
			for (int32_t tp : oldTPs) {
				if (rdx[tpx[tp]] != rdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::GT:
			for (int32_t tp : oldTPs) {
				if (rdx[tpx[tp]] > rdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::LE:
			for (int32_t tp : oldTPs) {
				if (rdx[tpx[tp]] <= rdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::LT:
			for (int32_t tp : oldTPs) {
				if (rdx[tpx[tp]] < rdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		case Predicate::Operator::GE:
			for (int32_t tp : oldTPs) {
				if (rdx[tpx[tp]] >= rdy[tpy[tp]])
					newTPs[newLength++] = tp;
			}
			break;
		default:
			assert(false && "Operador invalido para REAL");
		}
		break;
	}
	default:
		assert(false && "Tipo de coluna desconhecido");
	}

	newTPSubset->resize(newLength);
	return std::move(newTPSubset);
}

std::unique_ptr<TPSubSet> DuckDBDataset::sample(int32_t n) {
	auto tpSet = std::make_shared<TPSet>(n);
	std::uniform_int_distribution<int32_t> dist_x(0, this->size - 1);
	std::uniform_int_distribution<int32_t> dist_y(0, this->size - 2);
	for (int i = 0; i < n; i++) {
		int32_t x = dist_x(random_engine);
		int32_t y = dist_y(random_engine);
		if (y >= x)
			y++;
		tpSet->x[i] = x;
		tpSet->y[i] = y;
	}
	return make_uniq<TPSubSet>(tpSet);
}

} // namespace duckdb
