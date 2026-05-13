-- INSTALL '/home/thomas/limaduck/mockdb/build/release/extension/duckteste/duckteste.duckdb_extension';
LOAD 'duckteste';

--CREATE TABLE students (
--    id INTEGER,
--    name VARCHAR,
--    grade DOUBLE
--);

--INSERT INTO students VALUES (1, 'Alice', 9.5), (2, 'Bob', 8.0);

--SELECT * FROM lima_discover('students');

.timer on
-- LOAD 'build/release/extension/duckteste/duckteste.duckdb_extension';
CREATE TABLE funcionarios AS SELECT * FROM read_csv_auto('testes/test_complex_lima.csv');
SELECT * FROM lima_discover('funcionarios');
--SELECT * FROM lima_discover('testes/test_complex_lima.csv');
