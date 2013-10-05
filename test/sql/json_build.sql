
SELECT build_json_array('a',1,'b',1.2,'c',true,'d',null,'e',json '{"x": 3, "y": [1,2,3]}');

SELECT build_json_object('a',1,'b',1.2,'c',true,'d',null,'e',json '{"x": 3, "y": [1,2,3]}');

SELECT build_json_object( 
       'a', build_json_object('b',false,'c',99), 
       'd', build_json_object('e',array[9,8,7]::int[],
           'f', (select row_to_json(r) from ( select relkind, oid::regclass as name from pg_class where relname = 'pg_class') r)));


-- empty objects/arrays
SELECT build_json_array();

SELECT build_json_object();

-- make sure keys are quoted
SELECT build_json_object(1,2);

-- keys must be scalar and not null
SELECT build_json_object(null,2);

SELECT build_json_object(r,2) FROM (SELECT 1 AS a, 2 AS b) r;

SELECT build_json_object(json '{"a":1,"b":2}', 3);

SELECT build_json_object('{1,2,3}'::int[], 3);

CREATE TEMP TABLE foo (serial int, name text, type text);
INSERT INTO foo VALUES (847001,'t15','GE1043');
INSERT INTO foo VALUES (847002,'t16','GE1043');
INSERT INTO foo VALUES (847003,'sub-alpha','GESS90');

SELECT build_json_object('turbines',json_object_agg(serial,build_json_object('name',name,'type',type)))
FROM foo;

