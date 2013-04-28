
SELECT build_json_array('a',1,'b',1.2,'c',true,'d',null,'e',json '{"x": 3, "y": [1,2,3]}');

SELECT build_json_object('a',1,'b',1.2,'c',true,'d',null,'e',json '{"x": 3, "y": [1,2,3]}');

SELECT build_json_object( 
       'a', build_json_object('b',false,'c',99), 
       'd', build_json_object('e',array[9,8,7]::int[],
           'f', (select row_to_json(r) from ( select relkind, oid::regclass as name from pg_class where relname = 'pg_class') r)));

