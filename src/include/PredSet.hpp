#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <sstream>
#include <cassert>
#include <utility>
#include <algorithm>

namespace duckdb {

/**
 * Flat PredSet implementation:
 * - Replaces std::unordered_map<int32_t, int32_t> with a flat sorted inline buffer
 *   (plus overflow vector for >16 predicates) and a 64-bit column-pair presence bitmask.
 * - Shares predProd via a const pointer instead of deep-copying std::vector<int32_t>
 *   on every PredSet copy during Lattice::fetchTo / fetchFrom.
 */
class PredSet {
public:
	static constexpr int32_t MOD = 333333313;
	static constexpr size_t INLINE_CAP = 16;
	using Entry = std::pair<int32_t, int32_t>;

private:
	Entry inline_preds_[INLINE_CAP];
	std::vector<Entry> overflow_preds_;
	uint32_t size_ = 0;
	int32_t hash_val_ = 0;
	uint64_t cp_mask_ = 0;
	const std::vector<int32_t> *predProd_ = nullptr;

	Entry *data_ptr() {
		return (size_ <= INLINE_CAP) ? inline_preds_ : overflow_preds_.data();
	}

	const Entry *data_ptr() const {
		return (size_ <= INLINE_CAP) ? inline_preds_ : overflow_preds_.data();
	}

public:
	// Construtor inicial
	explicit PredSet(const std::vector<int32_t> &predProd_ref)
	    : size_(0), hash_val_(0), cp_mask_(0), predProd_(&predProd_ref) {
	}

	// Construtor de cópia (zero heap allocation for typical DC arities <= 16)
	PredSet(const PredSet &source)
	    : size_(source.size_), hash_val_(source.hash_val_), cp_mask_(source.cp_mask_), predProd_(source.predProd_) {
		if (size_ <= INLINE_CAP) {
			for (uint32_t i = 0; i < size_; ++i) {
				inline_preds_[i] = source.inline_preds_[i];
			}
		} else {
			overflow_preds_ = source.overflow_preds_;
		}
	}

	PredSet &operator=(const PredSet &source) {
		if (this != &source) {
			size_ = source.size_;
			hash_val_ = source.hash_val_;
			cp_mask_ = source.cp_mask_;
			predProd_ = source.predProd_;
			if (size_ <= INLINE_CAP) {
				overflow_preds_.clear();
				for (uint32_t i = 0; i < size_; ++i) {
					inline_preds_[i] = source.inline_preds_[i];
				}
			} else {
				overflow_preds_ = source.overflow_preds_;
			}
		}
		return *this;
	}

	// Calcula o score de um predicado individual
	int32_t PredScore(int32_t cp, int32_t p) const {
		return (p + 1) * (*predProd_)[cp];
	}

	// Verifica presença de um column pair em O(1) para cp < 64
	bool Contains(int32_t cp) const {
		if (cp >= 0 && cp < 64) {
			return ((cp_mask_ >> cp) & 1ULL) != 0;
		}
		const Entry *ptr = data_ptr();
		for (uint32_t i = 0; i < size_; ++i) {
			if (ptr[i].first == cp)
				return true;
			if (ptr[i].first > cp)
				break;
		}
		return false;
	}

	// Adiciona um predicado mantendo ordenação por cp e atualiza o hash incrementalmente
	void Add(int32_t cp, int32_t p) {
		assert(!Contains(cp));
		if (cp >= 0 && cp < 64) {
			cp_mask_ |= (1ULL << cp);
		}

		if (size_ < INLINE_CAP) {
			uint32_t pos = size_;
			while (pos > 0 && inline_preds_[pos - 1].first > cp) {
				inline_preds_[pos] = inline_preds_[pos - 1];
				--pos;
			}
			inline_preds_[pos] = {cp, p};
			++size_;
		} else if (size_ == INLINE_CAP) {
			overflow_preds_.reserve(INLINE_CAP * 2);
			overflow_preds_.assign(inline_preds_, inline_preds_ + INLINE_CAP);
			auto it = std::lower_bound(overflow_preds_.begin(), overflow_preds_.end(), Entry {cp, 0},
			                           [](const Entry &a, const Entry &b) { return a.first < b.first; });
			overflow_preds_.insert(it, {cp, p});
			++size_;
		} else {
			auto it = std::lower_bound(overflow_preds_.begin(), overflow_preds_.end(), Entry {cp, 0},
			                           [](const Entry &a, const Entry &b) { return a.first < b.first; });
			overflow_preds_.insert(it, {cp, p});
			++size_;
		}

		hash_val_ = (hash_val_ + PredScore(cp, p)) % MOD;
	}

	// Remove um predicado e reverte o hash
	void Remove(int32_t cp, int32_t p) {
		assert(Contains(cp));
		if (cp >= 0 && cp < 64) {
			cp_mask_ &= ~(1ULL << cp);
		}

		if (size_ <= INLINE_CAP) {
			uint32_t pos = 0;
			while (pos < size_ && inline_preds_[pos].first != cp) {
				++pos;
			}
			assert(pos < size_ && inline_preds_[pos].second == p);
			for (uint32_t i = pos; i + 1 < size_; ++i) {
				inline_preds_[i] = inline_preds_[i + 1];
			}
			--size_;
		} else {
			auto it = std::lower_bound(overflow_preds_.begin(), overflow_preds_.end(), Entry {cp, 0},
			                           [](const Entry &a, const Entry &b) { return a.first < b.first; });
			assert(it != overflow_preds_.end() && it->first == cp && it->second == p);
			overflow_preds_.erase(it);
			--size_;
			if (size_ == INLINE_CAP) {
				for (uint32_t i = 0; i < INLINE_CAP; ++i) {
					inline_preds_[i] = overflow_preds_[i];
				}
				overflow_preds_.clear();
			}
		}

		hash_val_ = (hash_val_ - PredScore(cp, p) + MOD) % MOD;
	}

	int32_t Get(int32_t cp) const {
		if (cp >= 0 && cp < 64 && (((cp_mask_ >> cp) & 1ULL) == 0)) {
			return -1;
		}
		const Entry *ptr = data_ptr();
		for (uint32_t i = 0; i < size_; ++i) {
			if (ptr[i].first == cp)
				return ptr[i].second;
			if (ptr[i].first > cp)
				break;
		}
		return -1;
	}

	size_t Size() const {
		return size_;
	}

	int32_t GetHashCode() const {
		return hash_val_;
	}

	bool operator==(const PredSet &other) const {
		if (hash_val_ != other.hash_val_ || size_ != other.size_ || cp_mask_ != other.cp_mask_) {
			return false;
		}
		const Entry *a = data_ptr();
		const Entry *b = other.data_ptr();
		for (uint32_t i = 0; i < size_; ++i) {
			if (a[i] != b[i])
				return false;
		}
		return true;
	}

	const Entry *begin() const {
		return data_ptr();
	}

	const Entry *end() const {
		return data_ptr() + size_;
	}

	std::string ToString() const {
		std::stringstream ss;
		ss << "{";
		const Entry *ptr = data_ptr();
		for (uint32_t i = 0; i < size_; ++i) {
			if (i > 0)
				ss << ", ";
			ss << ptr[i].first << "=" << ptr[i].second;
		}
		ss << "}";
		return ss.str();
	}
};

} // namespace duckdb

namespace std {
template <>
struct hash<duckdb::PredSet> {
	size_t operator()(const duckdb::PredSet &ps) const {
		return static_cast<size_t>(ps.GetHashCode());
	}
};
} // namespace std
