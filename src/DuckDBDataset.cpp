#include "DuckDBDataset.hpp"
#include "Schema.hpp"
#include "TPSubSet.hpp"
#include "TPSet.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/common/helper.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
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

	std::vector<std::string> colnames;
	std::vector<Column::Type> coltypes;

	for (auto &col : table_entry.GetColumns().Logical()) {
		colnames.push_back(col.GetName());

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

	Schema *s = new Schema(colnames, coltypes);

	return s;
}

DuckDBDataset::DuckDBDataset(ClientContext &context, std::string table_name, int32_t num_lines)
    : RelationalDataset(BuildDuckSchema(context, table_name), 0) {

	random_engine.seed(42);

	auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);
	auto &entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, table_name);
	auto &table_entry = entry.Cast<TableCatalogEntry>();

	int64_t total_rows = table_entry.GetStorage().GetTotalRows();
	this->size = (num_lines > 0 && num_lines < total_rows) ? num_lines : total_rows;
	int32_t n = this->size;

	this->constColumn.assign(schema->columns.size(), true);

	// --- 1. ALOCAR ESPAÇO PARA OS DADOS ---
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

		if (current_row + chunk_size > (idx_t)n) {
			chunk_size = n - current_row;
		}

		for (size_t i = 0; i < intCols.size(); i++) {
			int32_t colID = intCols[i]->ID;
			for (idx_t r = 0; r < chunk_size; r++) {
				auto val = scan_chunk.GetValue(colID, r);
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
		if (current_row >= (idx_t)n)
			break;
	}
}

// --- CONSTRUÇÃO / CACHE DE BITVECTORS DE 64 BITS (uint64_t) POR PREDICADO ---
const std::vector<uint64_t> &DuckDBDataset::getOrBuildPredBitvector(TPSet &source, Predicate *pred,
                                                                    int32_t &out_popcnt) {
	const size_t num_preds = schema->preds.size();
	if (source.pred_bits.empty()) {
		source.pred_bits.resize(num_preds);
		source.pred_popcnt.assign(num_preds, -1);
	}

	const int32_t pid = pred->ID;
	if (source.pred_popcnt[pid] >= 0) {
		out_popcnt = source.pred_popcnt[pid];
		return source.pred_bits[pid];
	}

	const int32_t n = source.length;
	const int32_t num_words = (n + 63) >> 6;
	const uint64_t last_mask = (n & 63) ? ((1ULL << (n & 63)) - 1ULL) : ~0ULL;

	// Se o predicado complementar no mesmo par de colunas (para STRING/INTEGER sem NaN) já foi
	// avaliado neste batch, podemos derivar o bitvector invertendo (~) as palavras de 64 bits!
	if (pred->cols->type == Column::Type::STRING || pred->cols->type == Column::Type::INTEGER) {
		int32_t comp_pID = -1;
		switch (pred->op) {
		case Predicate::Operator::EQ:
			comp_pID = 1; // NE
			break;
		case Predicate::Operator::NE:
			comp_pID = 0; // EQ
			break;
		case Predicate::Operator::GT:
			comp_pID = 3; // LE
			break;
		case Predicate::Operator::LE:
			comp_pID = 2; // GT
			break;
		case Predicate::Operator::LT:
			comp_pID = 5; // GE
			break;
		case Predicate::Operator::GE:
			comp_pID = 4; // LT
			break;
		}
		if (comp_pID >= 0 && (size_t)comp_pID < pred->cols->preds.size()) {
			int32_t comp_global_id = pred->cols->preds[comp_pID]->ID;
			if (source.pred_popcnt[comp_global_id] >= 0) {
				const auto &comp_bv = source.pred_bits[comp_global_id];
				auto &bv = source.pred_bits[pid];
				bv.resize(num_words);
				for (int32_t w = 0; w < num_words - 1; ++w) {
					bv[w] = ~comp_bv[w];
				}
				if (num_words > 0) {
					bv[num_words - 1] = (~comp_bv[num_words - 1]) & last_mask;
				}
				out_popcnt = n - source.pred_popcnt[comp_global_id];
				source.pred_popcnt[pid] = out_popcnt;
				return bv;
			}
		}
	}

	auto &bv = source.pred_bits[pid];
	bv.assign(num_words, 0ULL);

	const int32_t *__restrict tpx = source.x.data();
	const int32_t *__restrict tpy = source.y.data();
	int32_t popcnt = 0;

	auto eval_blocks = [&](auto cmp_fn) {
		int32_t full_words = n >> 6;
		for (int32_t w = 0; w < full_words; ++w) {
			const int32_t base = w << 6;
			uint64_t word = 0ULL;
			for (int32_t b = 0; b < 64; ++b) {
				int32_t tp = base + b;
				word |= (uint64_t(cmp_fn(tpx[tp], tpy[tp])) << b);
			}
			bv[w] = word;
			popcnt += __builtin_popcountll(word);
		}
		const int32_t rem = n & 63;
		if (rem > 0) {
			const int32_t base = full_words << 6;
			uint64_t word = 0ULL;
			for (int32_t b = 0; b < rem; ++b) {
				int32_t tp = base + b;
				word |= (uint64_t(cmp_fn(tpx[tp], tpy[tp])) << b);
			}
			bv[full_words] = word;
			popcnt += __builtin_popcountll(word);
		}
	};

	switch (pred->cols->type) {
	case Column::Type::STRING: {
		const int32_t *__restrict sdx = stringData[pred->cols->x->typeID].data();
		const int32_t *__restrict sdy = stringData[pred->cols->y->typeID].data();
		switch (pred->op) {
		case Predicate::Operator::EQ:
			eval_blocks([&](int32_t rx, int32_t ry) { return sdx[rx] == sdy[ry]; });
			break;
		case Predicate::Operator::NE:
			eval_blocks([&](int32_t rx, int32_t ry) { return sdx[rx] != sdy[ry]; });
			break;
		default:
			assert(false && "Operador invalido para STRING");
		}
		break;
	}
	case Column::Type::INTEGER: {
		const int32_t *__restrict idx = intData[pred->cols->x->typeID].data();
		const int32_t *__restrict idy = intData[pred->cols->y->typeID].data();
		switch (pred->op) {
		case Predicate::Operator::EQ:
			eval_blocks([&](int32_t rx, int32_t ry) { return idx[rx] == idy[ry]; });
			break;
		case Predicate::Operator::NE:
			eval_blocks([&](int32_t rx, int32_t ry) { return idx[rx] != idy[ry]; });
			break;
		case Predicate::Operator::GT:
			eval_blocks([&](int32_t rx, int32_t ry) { return idx[rx] > idy[ry]; });
			break;
		case Predicate::Operator::LE:
			eval_blocks([&](int32_t rx, int32_t ry) { return idx[rx] <= idy[ry]; });
			break;
		case Predicate::Operator::LT:
			eval_blocks([&](int32_t rx, int32_t ry) { return idx[rx] < idy[ry]; });
			break;
		case Predicate::Operator::GE:
			eval_blocks([&](int32_t rx, int32_t ry) { return idx[rx] >= idy[ry]; });
			break;
		default:
			assert(false && "Operador invalido para INTEGER");
		}
		break;
	}
	case Column::Type::REAL: {
		const float *__restrict rdx = realData[pred->cols->x->typeID].data();
		const float *__restrict rdy = realData[pred->cols->y->typeID].data();
		switch (pred->op) {
		case Predicate::Operator::EQ:
			eval_blocks([&](int32_t rx, int32_t ry) { return rdx[rx] == rdy[ry]; });
			break;
		case Predicate::Operator::NE:
			eval_blocks([&](int32_t rx, int32_t ry) { return rdx[rx] != rdy[ry]; });
			break;
		case Predicate::Operator::GT:
			eval_blocks([&](int32_t rx, int32_t ry) { return rdx[rx] > rdy[ry]; });
			break;
		case Predicate::Operator::LE:
			eval_blocks([&](int32_t rx, int32_t ry) { return rdx[rx] <= rdy[ry]; });
			break;
		case Predicate::Operator::LT:
			eval_blocks([&](int32_t rx, int32_t ry) { return rdx[rx] < rdy[ry]; });
			break;
		case Predicate::Operator::GE:
			eval_blocks([&](int32_t rx, int32_t ry) { return rdx[rx] >= rdy[ry]; });
			break;
		default:
			assert(false && "Operador invalido para REAL");
		}
		break;
	}
	default:
		assert(false && "Tipo de coluna desconhecido");
	}

	source.pred_popcnt[pid] = popcnt;
	out_popcnt = popcnt;
	return bv;
}

// --- 3. FILTRO (Com 1-Line TPSubSet Fix + Bitvectors de 64 bits uint64_t fatiados por [min_word, max_word]) ---
std::unique_ptr<TPSubSet> DuckDBDataset::filter(TPSubSet &TPs, Predicate *pred) {
	// 1-LINE FIX: constrói a partir de `TPs` (e não de `TPs.source`)!
	auto newTPSubset = make_uniq<TPSubSet>(TPs);

	if (TPs.length == 0) {
		newTPSubset->length = 0;
		newTPSubset->min_word = 0;
		newTPSubset->max_word = -1;
		return newTPSubset;
	}

	// Caso 1: Filtro de Nível 1 sobre o TPSet raiz (is_full == true)
	if (TPs.is_full) {
		int32_t popcnt = 0;
		const auto &pred_bv = getOrBuildPredBitvector(*TPs.source, pred, popcnt);
		const int32_t num_words = (TPs.source->length + 63) >> 6;

		newTPSubset->length = popcnt;
		if (popcnt == 0) {
			newTPSubset->min_word = 0;
			newTPSubset->max_word = -1;
			return newTPSubset;
		}

		if (popcnt < num_words) {
			newTPSubset->has_bits = false;
			newTPSubset->TPs.resize(popcnt);
			int32_t idx = 0;
			for (int32_t w = 0; w < num_words; ++w) {
				uint64_t m = pred_bv[w];
				while (m) {
					int bit = __builtin_ctzll(m);
					newTPSubset->TPs[idx++] = (w << 6) + bit;
					m &= (m - 1);
				}
			}
		} else {
			int32_t first_nz = 0;
			while (first_nz < num_words && pred_bv[first_nz] == 0ULL)
				++first_nz;
			int32_t last_nz = num_words - 1;
			while (last_nz >= first_nz && pred_bv[last_nz] == 0ULL)
				--last_nz;
			newTPSubset->min_word = first_nz;
			newTPSubset->max_word = last_nz;
			newTPSubset->has_bits = true;
			newTPSubset->bits.assign(pred_bv.begin() + first_nz, pred_bv.begin() + last_nz + 1);
		}
		return newTPSubset;
	}

	// Caso 2: Interseção de Bitvectors de 64 bits (AND + __builtin_popcountll) na DFS
	if (TPs.has_bits) {
		int32_t dummy_popcnt = 0;
		const auto &pred_bv = getOrBuildPredBitvector(*TPs.source, pred, dummy_popcnt);
		const int32_t min_w = TPs.min_word;
		const int32_t max_w = TPs.max_word;
		const int32_t active_span = max_w - min_w + 1;

		if (active_span <= 0) {
			newTPSubset->length = 0;
			return newTPSubset;
		}

		const uint64_t *__restrict p_bits = TPs.bits.data();
		const uint64_t *__restrict e_bits = pred_bv.data() + min_w;

		int32_t newLength = 0;
		int32_t first_rel = -1;
		int32_t last_rel = -2;

		for (int32_t i = 0; i < active_span; ++i) {
			uint64_t m = p_bits[i] & e_bits[i];
			if (m) {
				newLength += __builtin_popcountll(m);
				if (first_rel < 0)
					first_rel = i;
				last_rel = i;
			}
		}

		newTPSubset->length = newLength;
		if (newLength == 0) {
			newTPSubset->min_word = 0;
			newTPSubset->max_word = -1;
			newTPSubset->has_bits = false;
			return newTPSubset;
		}

		const int32_t new_span = last_rel - first_rel + 1;
		newTPSubset->min_word = min_w + first_rel;
		newTPSubset->max_word = min_w + last_rel;

		if (newLength < new_span) {
			newTPSubset->TPs.resize(newLength);
			int32_t idx = 0;
			for (int32_t i = first_rel; i <= last_rel; ++i) {
				uint64_t m = p_bits[i] & e_bits[i];
				const int32_t base_tp = (min_w + i) << 6;
				while (m) {
					int bit = __builtin_ctzll(m);
					newTPSubset->TPs[idx++] = base_tp + bit;
					m &= (m - 1);
				}
			}
			newTPSubset->has_bits = false;
		} else {
			newTPSubset->bits.resize(new_span);
			uint64_t *__restrict out_bits = newTPSubset->bits.data();
			for (int32_t i = first_rel; i <= last_rel; ++i) {
				out_bits[i - first_rel] = p_bits[i] & e_bits[i];
			}
			newTPSubset->has_bits = true;
		}
		return newTPSubset;
	}

	// Caso 3: Subconjunto já esparso (TPs.TPs com menos de 1 elemento por palavra de 64 bits)
	const auto &oldTPs = TPs.TPs;
	auto &newTPs = newTPSubset->TPs;
	int32_t newLength = 0;

	if (!TPs.source->pred_popcnt.empty() && (size_t)pred->ID < TPs.source->pred_popcnt.size() &&
	    TPs.source->pred_popcnt[pred->ID] >= 0) {
		const uint64_t *__restrict e_bits = TPs.source->pred_bits[pred->ID].data();
		for (int32_t tp : oldTPs) {
			if ((e_bits[tp >> 6] >> (tp & 63)) & 1ULL) {
				newTPs[newLength++] = tp;
			}
		}
		newTPSubset->resize(newLength);
		return newTPSubset;
	}

	const auto &tpx = TPs.source->x;
	const auto &tpy = TPs.source->y;

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
	return newTPSubset;
}

// --- 4. AMOSTRAGEM ULTRARRÁPIDA COM XOSHIRO256++ & SUPORTE A CLUSTERS PLI (1-Const, 2-Const & Bipartite) ---
std::unique_ptr<TPSubSet> DuckDBDataset::sample(int32_t n) {
	auto tpSet = std::make_shared<TPSet>(n);
	int32_t *__restrict px = tpSet->x.data();
	int32_t *__restrict py = tpSet->y.data();

	// Caso A: Cluster Bipartido (t_x.A = c1 & t_y.A = c2, c1 != c2)
	if (this->activeCluster != nullptr && this->activeClusterY != nullptr) {
		const int32_t *__restrict rows_x = this->activeCluster->data();
		const int32_t *__restrict rows_y = this->activeClusterY->data();
		const uint64_t k_x = static_cast<uint64_t>(this->activeCluster->size());
		const uint64_t k_y = static_cast<uint64_t>(this->activeClusterY->size());
		for (int32_t i = 0; i < n; i++) {
			const uint64_t r = random_engine.next();
			const int32_t ix = static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(r)) * k_x) >> 32);
			const int32_t iy =
			    static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(r >> 32)) * k_y) >> 32);
			px[i] = rows_x[ix];
			py[i] = rows_y[iy];
		}
		return make_uniq<TPSubSet>(tpSet);
	}

	// Caso B: Cluster Homo-Constante (1-Constante ou 2-Constantes Conjuntivas)
	if (this->activeCluster != nullptr) {
		const int32_t *__restrict cluster_rows = this->activeCluster->data();
		const uint64_t k_x = static_cast<uint64_t>(this->activeCluster->size());
		const uint64_t k_y = k_x - 1ULL;
		for (int32_t i = 0; i < n; i++) {
			const uint64_t r = random_engine.next();
			const int32_t ix = static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(r)) * k_x) >> 32);
			int32_t iy = static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(r >> 32)) * k_y) >> 32);
			if (iy >= ix)
				iy++;
			px[i] = cluster_rows[ix];
			py[i] = cluster_rows[iy];
		}
		return make_uniq<TPSubSet>(tpSet);
	}

	// Caso C: Amostragem Global Padrão
	const uint64_t size_x = static_cast<uint64_t>(this->size);
	const uint64_t size_y = static_cast<uint64_t>(this->size - 1);

	for (int32_t i = 0; i < n; i++) {
		const uint64_t r = random_engine.next();
		const int32_t x = static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(r)) * size_x) >> 32);
		int32_t y = static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(r >> 32)) * size_y) >> 32);
		if (y >= x)
			y++;
		px[i] = x;
		py[i] = y;
	}
	return make_uniq<TPSubSet>(tpSet);
}

// --- 5. CONSTRUÇÃO DE CLUSTERS PLI (1-Constante, 2-Constantes Conjuntivas e Bipartidos) ---
std::vector<ConstantCluster> DuckDBDataset::buildConstantClusters(double min_support) {
	std::vector<ConstantCluster> base_clusters;
	if (this->size < 2)
		return base_clusters;

	const int32_t min_rows = std::max<int32_t>(2, static_cast<int32_t>(std::ceil(min_support * this->size)));

	// 1. Clusters Base de 1-Constante em Colunas STRING
	auto &strCols = schema->typeColumns[Column::Type::STRING];
	int32_t empty_id = 0;
	auto empty_it = stringMap.find("");
	if (empty_it != stringMap.end()) {
		empty_id = empty_it->second;
	}

	for (size_t i = 0; i < strCols.size(); ++i) {
		Column *col = strCols[i];
		if (constColumn[col->ID])
			continue;

		const auto &col_data = stringData[i];
		std::unordered_map<int32_t, std::vector<int32_t>> pli;
		for (int32_t r = 0; r < this->size; ++r) {
			int32_t v = col_data[r];
			if (v == 0 || v == empty_id)
				continue;
			pli[v].push_back(r);
		}

		for (auto &kv : pli) {
			int32_t count = static_cast<int32_t>(kv.second.size());
			if (count >= min_rows && count < this->size) {
				std::string val_str =
				    (kv.first >= 1 && (size_t)kv.first <= invStringMap.size()) ? invStringMap[kv.first - 1] : std::to_string(kv.first);
				ConstantCluster cc;
				cc.ignored_cols = {col->ID};
				cc.condition_str = col->name + " == '" + val_str + "'";
				cc.row_ids = std::move(kv.second);
				cc.is_bipartite = false;
				base_clusters.push_back(std::move(cc));
			}
		}
	}

	// 2. Clusters Base de 1-Constante em Colunas INTEGER
	auto &intCols = schema->typeColumns[Column::Type::INTEGER];
	for (size_t i = 0; i < intCols.size(); ++i) {
		Column *col = intCols[i];
		if (constColumn[col->ID])
			continue;

		const auto &col_data = intData[i];
		std::unordered_map<int32_t, std::vector<int32_t>> pli;
		for (int32_t r = 0; r < this->size; ++r) {
			pli[col_data[r]].push_back(r);
		}

		for (auto &kv : pli) {
			int32_t count = static_cast<int32_t>(kv.second.size());
			if (count >= min_rows && count < this->size) {
				ConstantCluster cc;
				cc.ignored_cols = {col->ID};
				cc.condition_str = col->name + " == " + std::to_string(kv.first);
				cc.row_ids = std::move(kv.second);
				cc.is_bipartite = false;
				base_clusters.push_back(std::move(cc));
			}
		}
	}

	// Ordena os clusters de 1-constante por suporte decrescente
	std::sort(base_clusters.begin(), base_clusters.end(), [](const ConstantCluster &a, const ConstantCluster &b) {
		if (a.row_ids.size() != b.row_ids.size())
			return a.row_ids.size() > b.row_ids.size();
		if (a.ignored_cols[0] != b.ignored_cols[0])
			return a.ignored_cols[0] < b.ignored_cols[0];
		return a.condition_str < b.condition_str;
	});

	std::vector<ConstantCluster> all_clusters = base_clusters;
	const size_t num_base = base_clusters.size();

	// 3. Clusters de 2-Constantes Conjuntivas (ColA == c1 & ColB == c2, ColA != ColB)
	// e Clusters Bipartidos Cross-Tuple (t1.ColA == c1 & t2.ColA == c2, c1 != c2 na mesma coluna)
	for (size_t i = 0; i < num_base; ++i) {
		const auto &c1 = base_clusters[i];
		const int32_t col1 = c1.ignored_cols[0];

		for (size_t j = i + 1; j < num_base; ++j) {
			const auto &c2 = base_clusters[j];
			const int32_t col2 = c2.ignored_cols[0];

			if (col1 == col2) {
				// Mesma coluna, valores constantes distintos (c1 != c2): Cluster Bipartido (t1.A = c1 & t2.A = c2)
				double geom_support =
				    std::sqrt(static_cast<double>(c1.row_ids.size()) * static_cast<double>(c2.row_ids.size()));
				if (geom_support >= min_rows) {
					ConstantCluster bip;
					bip.ignored_cols = {col1};
					bip.condition_str = "t1." + c1.condition_str + " & t2." + c2.condition_str;
					bip.row_ids = c1.row_ids;
					bip.row_ids_y = c2.row_ids;
					bip.is_bipartite = true;
					bip.parent_cluster_indices = {static_cast<int32_t>(i), static_cast<int32_t>(j)};
					all_clusters.push_back(std::move(bip));
				}
			} else {
				// Colunas distintas (col1 != col2): Cluster de 2-Constantes Conjuntivas via interseção PLI O(|c1|+|c2|)
				if (static_cast<int32_t>(std::min(c1.row_ids.size(), c2.row_ids.size())) < min_rows)
					continue;

				std::vector<int32_t> inter;
				inter.reserve(std::min(c1.row_ids.size(), c2.row_ids.size()));
				std::set_intersection(c1.row_ids.begin(), c1.row_ids.end(), c2.row_ids.begin(), c2.row_ids.end(),
				                      std::back_inserter(inter));

				int32_t inter_count = static_cast<int32_t>(inter.size());
				// Descarta se não atinge min_rows OU se a interseção é 100% idêntica a um dos pais
				// (pois nesse caso uma constante implica deterministicamente a outra e não adiciona informação!)
				if (inter_count >= min_rows && inter_count < static_cast<int32_t>(c1.row_ids.size()) &&
				    inter_count < static_cast<int32_t>(c2.row_ids.size())) {
					ConstantCluster cc2;
					cc2.ignored_cols = {col1, col2};
					cc2.condition_str = c1.condition_str + " & " + c2.condition_str;
					cc2.row_ids = std::move(inter);
					cc2.is_bipartite = false;
					cc2.parent_cluster_indices = {static_cast<int32_t>(i), static_cast<int32_t>(j)};
					all_clusters.push_back(std::move(cc2));
				}
			}
		}
	}

	return all_clusters;
}

} // namespace duckdb
