# LIMA Extension for DuckDB

This is a native C++ extension for DuckDB that implements the **LIMA** algorithm for automatic Denial Constraint (DC) discovery directly within the database engine.

## Prerequisites

To compile the extension from source, you will need:

*   **CMake** (version 3.20 or higher)
*   **C++ Compiler** with C++11 support (GCC, Clang, or MSVC on Windows)
*   **Git**
*   **Python 3** (optional, for running benchmark scripts)
*   **Make** (or Ninja)

---

## Compilation Guide

### 1. Clone the Repository

Since the extension depends on the DuckDB source code as a submodule, you must clone recursively:

```bash
git clone --recursive <YOUR_REPO_URL> limaduck
cd limaduck/mockdb
```

*If you already cloned without submodules, initialize them manually:*
```bash
git submodule update --init --recursive
```

### 2. Build the Extension (Release Mode)

For maximum performance (essential for search-heavy algorithms like LIMA), you **must** build in `release` mode. 

Run the following command in the `mockdb` directory:

```bash
make release
```

*Note:* A simple `make` command will create a *Debug* build, which is extremely slow and intended only for development. **Do not use Debug builds for real datasets or benchmarks.**

The compiled extension file will be generated at:
`build/release/extension/lima/lima.duckdb_extension`

---

## Usage Guide

### Using with an External DuckDB (Python, CLI, or GUI)

If you already have DuckDB installed (e.g., via `pip install duckdb` or a system package), you can load this extension **without** using the DuckDB binary built by the `make` command, provided the versions match.

**Important: Version Matching**
DuckDB extensions are tied to specific versions. For this extension to load, your installed DuckDB version must match the version in the `mockdb/duckdb` submodule (currently **v1.5.3**).

In your DuckDB environment (CLI or Python), run:

```sql
-- 1. Enable loading of unsigned/local extensions
SET allow_unsigned_extensions=true;

-- 2. Install using the ABSOLUTE path to the .duckdb_extension file
INSTALL '/path/to/limaduck/mockdb/build/release/extension/lima/lima.duckdb_extension';

-- 3. Load the extension
LOAD 'lima';

-- 4. Run discovery
SELECT * FROM lima_discover('my_table');
```

### Using via Python Example

```python
import duckdb

# Connect and allow local extensions
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})

# Path to the extension file you compiled
EXT_PATH = '/home/user/limaduck/mockdb/build/release/extension/lima/lima.duckdb_extension'

con.execute(f"INSTALL '{EXT_PATH}';")
con.execute("LOAD 'lima';")

# The extension works on both Tables and raw CSV files
results = con.execute("SELECT * FROM lima_discover('data.csv', threshold=0.01);").fetchall()
print(results)
```

---

## Smart Features

*   **Direct File Loading:** The `lima_discover` function is "smart." If you pass a string that isn't a registered table name, it will attempt to treat it as a file path and use DuckDB's `read_csv_auto` to create a temporary table automatically.