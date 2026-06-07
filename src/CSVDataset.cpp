#include "CSVDataset.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include "duckdb/common/helper.hpp"
#include <string>
#include "Schema.hpp"
#include <iostream>

namespace duckdb {

// Função auxiliar para dividir uma string com base em um delimitador
std::vector<std::string> split(const std::string &s, char delimiter) {
	std::vector<std::string> tokens;
	std::string token;
	std::stringstream tokenStream(s);

	// Lê a stream até encontrar o delimitador e extrai o token
	while (std::getline(tokenStream, token, delimiter)) {
		tokens.push_back(token);
	}

	return tokens;
}

CSVDataset::CSVDataset(const std::string &path, int32_t n_size, uint64_t seed) : RelationalDataset(nullptr, n_size) {
	random_engine.seed(seed);
	buildColumns(path);
}

void CSVDataset::buildColumns(const std::string &path) {
	std::ifstream file(path);
	std::string line;

	// std::cerr << "[DEBUG - LIMA] Tentando abrir o arquivo: " << path << "\n";

	if (!file.is_open()) {
		std::cerr << "[FATAL - LIMA] O arquivo CSV nao foi encontrado no caminho: " << path << "\n";
		return;
	}
	if (!std::getline(file, line)) {
		std::cerr << "[FATAL - LIMA] O arquivo CSV esta vazio!\n";
		return;
	}

	// std::cerr << "[DEBUG - LIMA] Header lido com sucesso: " << line << "\n";

	// 1. Ler Cabeçalhos e criar Schema
	std::vector<std::string> colnames = split(line, ',');
	// std::cerr << "[DEBUG - LIMA] Criando o Schema para " << colnames.size() << " colunas...\n";
	this->schema = new Schema(colnames);

	int32_t actual_lines = this->size;

	if (actual_lines <= 0) {
		// std::cerr << "[DEBUG - LIMA] Auto-detectando total de linhas no arquivo...\n";
		// Guarda a posição atual (logo após ler o cabeçalho)
		std::streampos oldpos = file.tellg();

		actual_lines = 0;
		std::string dummy;
		while (std::getline(file, dummy)) {
			if (!dummy.empty() && dummy != "\r") {
				actual_lines++;
			}
		}

		// Volta o leitor de arquivos para o início (logo após o cabeçalho)
		file.clear();
		file.seekg(oldpos);

		// Atualiza o tamanho na classe
		this->size = actual_lines;
		// std::cerr << "[DEBUG - LIMA] Arquivo contem " << actual_lines << " linhas de dados.\n";
	}

	int32_t n = this->size;
	this->constColumn.assign(schema->columns.size(), true);

	// 2. Alocar espaço para os dados (Equivalente ao reserve() que discutimos)
	size_t numInt = schema->typeColumns[Column::Type::INTEGER].size();
	intData.assign(numInt, std::vector<int32_t>(n));

	size_t numReal = schema->typeColumns[Column::Type::REAL].size();
	realData.assign(numReal, std::vector<float>(n));

	size_t numStr = schema->typeColumns[Column::Type::STRING].size();
	stringData.assign(numStr, std::vector<int32_t>(n));

	// 3. Loop de Parsing Completo em C++
	auto &intCols = schema->typeColumns[Column::Type::INTEGER];
	auto &realCols = schema->typeColumns[Column::Type::REAL];
	auto &strCols = schema->typeColumns[Column::Type::STRING];

	// std::cerr << "[DEBUG - LIMA] Alocando matrizes. Tipos -> Int: " << intCols.size() << " | Real: " <<
	// realCols.size() << " | Str: " << strCols.size() << "\n";

	for (int row = 0; row < n; ++row) {
		if (!std::getline(file, line))
			break;

		// Supondo a função split que discutimos anteriormente
		std::vector<std::string> fields = split(line, ',');

		// --- INTEIROS ---
		for (size_t i = 0; i < intCols.size(); i++) {
			int32_t colID = intCols[i]->ID;
			intData[i][row] = std::stoi(fields[colID]); // Integer.parseInt do C++

			if (constColumn[colID] && intData[i][row] != intData[i][0]) {
				constColumn[colID] = false;
			}
		}

		// --- REAIS (FLOAT) ---
		for (size_t i = 0; i < realCols.size(); i++) {
			int32_t colID = realCols[i]->ID;
			// Operador ternário para lidar com vazios (NaN)
			realData[i][row] =
			    fields[colID].empty() ? std::numeric_limits<float>::quiet_NaN() : std::stof(fields[colID]);

			if (constColumn[colID] && realData[i][row] != realData[i][0]) {
				constColumn[colID] = false;
			}
		}

		// --- STRINGS (Dictionary Encoding) ---
		for (size_t i = 0; i < strCols.size(); i++) {
			int32_t colID = strCols[i]->ID;
			const std::string &k = fields[colID];

			// Equivalente otimizado ao putIfAbsent do Java
			auto it = stringMap.find(k);
			int32_t v;
			if (it == stringMap.end()) {
				v = stringMap.size() + 1; // Novo ID
				stringMap[k] = v;
				invStringMap.push_back(k);
			} else {
				v = it->second; // ID existente
			}

			stringData[i][row] = v;

			if (constColumn[colID] && stringData[i][row] != stringData[i][0]) {
				constColumn[colID] = false;
			}
		}
	}
}

std::vector<std::vector<std::string>> CSVDataset::get(int32_t n) {
	// 1. Cria o container de retorno (um vetor de vetores de strings)
	std::vector<std::vector<std::string>> res;

	// 2. Otimização: Reserva espaço para 'n' linhas de uma vez
	res.reserve(n);

	std::string line;
	for (int i = 0; i < n; i++) {
		// 3. Lê a próxima linha bruta do arquivo CSV
		if (!std::getline(this->file_stream, line))
			break;

		// 4. Divide a linha por vírgulas e adiciona ao resultado
		// Nota: Você precisará de uma função auxiliar 'split'
		res.push_back(split(line, ','));
	}

	return res;
}

std::unique_ptr<TPSubSet> CSVDataset::filter(TPSubSet &TPs, Predicate *pred) {
	// Cria o novo subconjunto clonando a origem
	// auto newTPSubset = std::make_unique<TPSubSet>(*TPs.source);
	auto newTPSubset = make_uniq<TPSubSet>(TPs.source);

	// Atalhos (referências) para evitar cópias de memória
	const auto &tpx = TPs.source->x;
	const auto &tpy = TPs.source->y;
	const auto &oldTPs = TPs.TPs;
	auto &newTPs = newTPSubset->TPs;

	int32_t newLength = 0;

	// Primeiro Nível: Tipo de Dado
	switch (pred->cols->type) {

	case Column::Type::STRING: {
		// Referências de tempo O(1) para as colunas exatas
		const auto &sdx = stringData[pred->cols->x->typeID];
		const auto &sdy = stringData[pred->cols->y->typeID];

		// Segundo Nível (Branch Hoisting): Operador
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
			// Em C++, assert falso para depuração em compilações Debug
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

	// Ajusta o tamanho final do vetor para economizar memória
	newTPSubset->resize(newLength);
	return newTPSubset;
}

std::unique_ptr<TPSubSet> CSVDataset::sample(int32_t n) {
	auto tpSet = std::make_shared<TPSet>(n);
	std::uniform_int_distribution<int32_t> dist_x(0, this->size - 1);
	std::uniform_int_distribution<int32_t> dist_y(0, this->size - 2);

	for (int i = 0; i < n; i++) {
		int32_t x = dist_x(random_engine);
		int32_t y = dist_y(random_engine);
		if (y >= x)
			y++; // Garantia de que x != y
		tpSet->x[i] = x;
		tpSet->y[i] = y;
	}
	return make_uniq<TPSubSet>(tpSet);
}

} // namespace duckdb
