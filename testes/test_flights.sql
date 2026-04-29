-- INSTALL '/home/thomas/limaduck/mockdb/build/release/extension/duckteste/duckteste.duckdb_extension';
LOAD 'duckteste';

CREATE TABLE flights AS SELECT * FROM read_csv_auto('/home/thomas/limaduck/DCValidity/datasets/flights.csv');

SELECT * FROM lima_discover('flights');
