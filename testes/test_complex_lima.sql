-- INSTALL '/home/thomas/limaduck/mockdb/build/release/extension/lima/lima.duckdb_extension';
LOAD 'lima';

-- Create a more complex table to test dependencies
-- Rules to expect:
-- 1. !(t1.id = t2.id) -> ID is unique
-- 2. !(t1.city = t2.city & t1.state != t2.state) -> city implies state
-- 3. !(t1.zip = t2.zip & t1.city != t2.city) -> zip implies city
CREATE TABLE staff (
    id INTEGER,
    name VARCHAR,
    city VARCHAR,
    state VARCHAR,
    zip VARCHAR,
    salary INTEGER
);

INSERT INTO staff VALUES 
(1, 'Alice', 'New York', 'NY', '10001', 95000),
(2, 'Bob', 'New York', 'NY', '10001', 80000),
(3, 'Charlie', 'San Francisco', 'CA', '94105', 120000),
(4, 'David', 'San Francisco', 'CA', '94105', 110000),
(5, 'Eve', 'Los Angeles', 'CA', '90001', 105000),
(6, 'Frank', 'Los Angeles', 'CA', '90001', 90000),
(7, 'Grace', 'Miami', 'FL', '33101', 85000),
(8, 'Heidi', 'Miami', 'FL', '33101', 88000),
(9, 'Ivan', 'Chicago', 'IL', '60601', 92000),
(10, 'Jack', 'Chicago', 'IL', '60601', 94000),
(11, 'Karl', 'Boston', 'MA', '02108', 97000),
(12, 'Linda', 'Boston', 'MA', '02108', 99000);

-- Run discovery with a standard threshold
SELECT * FROM lima_discover('staff', 0.00001);
