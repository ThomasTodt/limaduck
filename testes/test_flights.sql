
-- INSTALL '/home/thomas/limaduck/mockdb/build/release/extension/lima/lima.duckdb_extension';
LOAD 'lima';

CREATE TABLE flights_20 AS SELECT * FROM read_csv_auto('/home/thomas/mestrado/DCValidity/datasets/flights.csv');

.timer on
SELECT * FROM lima_discover('flights_20', num_lines=10000, threshold=0.01);