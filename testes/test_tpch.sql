INSTALL tpch;
LOAD tpch;
LOAD 'lima';

DROP TABLE IF EXISTS customer;
DROP TABLE IF EXISTS lineitem;
DROP TABLE IF EXISTS nation;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS part;
DROP TABLE IF EXISTS partsupp;
DROP TABLE IF EXISTS region;
DROP TABLE IF EXISTS supplier;

CALL dbgen(sf = 1);
-- PRAGMA tpch(4);

-- select count(*) from customer;
-- select count(*) from lineitem;
-- select count(*) from nation;
-- select count(*) from orders;
-- select count(*) from part;
-- select count(*) from partsupp;
-- select count(*) from region;
-- select count(*) from supplier;

select * from lima_discover(customer, threshold=0.0000000001);
select * from lima_discover(lineitem, threshold=0.0000000001);
select * from lima_discover(nation, threshold=0.0000000001);
select * from lima_discover(orders, threshold=0.0000000001);
select * from lima_discover(part, threshold=0.0000000001);
select * from lima_discover(partsupp, threshold=0.0000000001);
select * from lima_discover(region, threshold=0.0000000001);
select * from lima_discover(supplier, threshold=0.0000000001);