// Em BetaDistribution.cpp
#include "BetaDistribution.hpp"
#include <cmath> // Para M_PI

namespace duckdb {

// Aqui nós instanciamos e populamos os vetores estáticos do C++11
std::vector<double> BetaDistribution::y1mem = {-BetaDistribution::yConst};
std::vector<double> BetaDistribution::y2mem = {std::pow(M_PI, 2) / 6.0};
std::vector<double> BetaDistribution::y3mem = {-2.404114};

} // namespace duckdb