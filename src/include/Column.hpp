#pragma once

#include <cstdint>
#include <string>

namespace duckdb {

/**
 * Representa uma coluna no esquema do LIMA.
 * Traduzido de edu.upc.data.schema.Column
 */
class Column {
public:
	// Enum class no C++ evita conflitos de nomes globais
	enum class Type { STRING, REAL, INTEGER };

	std::string name;
	Type type;
	int32_t ID;
	int32_t typeID;

	Column(const std::string &col_name) : name(col_name) {
		// Lógica de inferência de tipo baseada no nome (idêntica ao Java)
		if (name.find("Double") != std::string::npos) {
			type = Type::REAL;
		} else if (name.find("Integer") != std::string::npos) {
			type = Type::INTEGER;
		} else {
			type = Type::STRING;
		}
	}

	/**
	 * Equivalente ao equals() do Java.
	 * No C++, comparamos o valor das strings diretamente com ==.
	 */
	bool Equals(const Column &other) const {
		return type == other.type && name == other.name;
	}

	// Sobrecarga de operador para uso idiomático em C++
	bool operator==(const Column &other) const {
		return Equals(other);
	}
};

} // namespace duckdb
