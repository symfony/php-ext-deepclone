--TEST--
deepclone_to_array() produces correct array format
--EXTENSIONS--
deepclone
--FILE--
<?php

// ── Scalars ──
var_dump(deepclone_to_array(42) === ['value' => 42]);
var_dump(deepclone_to_array('hello') === ['value' => 'hello']);
var_dump(deepclone_to_array(true) === ['value' => true]);
var_dump(deepclone_to_array(false) === ['value' => false]);
var_dump(deepclone_to_array(null) === ['value' => null]);
var_dump(deepclone_to_array(3.14) === ['value' => 3.14]);
var_dump(deepclone_to_array([]) === ['value' => []]);

// ── Simple stdClass ──
$o = new stdClass();
$o->foo = 'bar';
$o->num = 42;
$d = deepclone_to_array($o);
var_dump($d['classes'] === 'stdClass');
var_dump($d['objectMeta'] === 1);
var_dump($d['prepared'] === 0);
var_dump($d['properties']['stdClass']['foo'] === [0 => 'bar']);
var_dump($d['properties']['stdClass']['num'] === [0 => 42]);

// ── Nested objects ──
$o = new stdClass();
$o->child = new stdClass();
$o->child->val = 'inner';
$d = deepclone_to_array($o);
var_dump($d['objectMeta'] === 2);
var_dump($d['properties']['stdClass']['val'] === [1 => 'inner']);
var_dump($d['resolve']['stdClass']['child'] === [0 => true]);

// ── Shared object reference ──
$child = new stdClass();
$child->x = 1;
$o = new stdClass();
$o->a = $child;
$o->b = $child;
$d = deepclone_to_array($o);
var_dump($d['properties']['stdClass']['a'][0] === $d['properties']['stdClass']['b'][0]);

// ── Circular reference ──
$a = new stdClass();
$b = new stdClass();
$a->ref = $b;
$b->ref = $a;
$d = deepclone_to_array($a);
var_dump($d['objectMeta'] === 2);
var_dump($d['resolve']['stdClass']['ref'][0] === true);
var_dump($d['resolve']['stdClass']['ref'][1] === true);

// ── Hard references (scalar) ──
$v = [1];
$v[] = &$v[0];
$d = deepclone_to_array($v);
var_dump($d['prepared'] === [-1, -1]);
var_dump($d['mask'] === [false, false]);
var_dump(isset($d['refs'][1]));

// ── Hard references (recursive) ──
$v = [];
$v[0] = &$v;
$d = deepclone_to_array($v);
var_dump($d['prepared'] === [-1]);
var_dump($d['mask'] === [false]);
var_dump(isset($d['refs'][1]));
var_dump(isset($d['refMasks'][1]));

// ── Unshared reference (becomes static) ──
$x = [123];
$d = deepclone_to_array([&$x]);
var_dump(array_key_exists('value', $d));
var_dump($d['value'] === [[123]]);

// ── Named closure (global function) ──
$d = deepclone_to_array(strlen(...), allow_named_closures: true);
var_dump($d['prepared'] === [null, 'strlen']);
var_dump($d['mask'] === 0);

// ── Large string (COW) ──
$big = str_repeat('x', 10000);
$o = new stdClass();
$o->data = $big;
$d = deepclone_to_array($o);
var_dump($d['properties']['stdClass']['data'][0] === $big);

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
