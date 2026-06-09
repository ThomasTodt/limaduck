#pragma once

#include <cstdint>
#include <string>

namespace duckdb {

class Column {
public:
	enum class Type { STRING, REAL, INTEGER };

	std::string name;
	Type type;
	int32_t ID;
	int32_t typeID;

	Column(const std::string &col_name) : name(col_name) {
		if (name.find("Double") != std::string::npos) {
			type = Type::REAL;
		} else if (name.find("Integer") != std::string::npos) {
			type = Type::INTEGER;
		} else {
			type = Type::STRING;
		}
	}

	bool Equals(const Column &other) const {
		return type == other.type && name == other.name;
	}

	bool operator==(const Column &other) const {
		return Equals(other);
	}
};

} // namespace duckdb
