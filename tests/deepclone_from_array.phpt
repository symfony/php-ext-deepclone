--TEST--
deepclone_from_array() reconstructs values correctly
--EXTENSIONS--
deepclone
--FILE--
<?php

// ── Scalars ──
var_dump(deepclone_from_array(['value' => 42]) === 42);
var_dump(deepclone_from_array(['value' => 'hello']) === 'hello');
var_dump(deepclone_from_array(['value' => null]) === null);
var_dump(deepclone_from_array(['value' => true]) === true);
var_dump(deepclone_from_array(['value' => [1, 2]]) === [1, 2]);

// ── Simple stdClass ──
$o = new stdClass();
$o->foo = 'bar';
$o->num = 42;
$clone = deepclone_from_array(deepclone_to_array($o));
var_dump($clone instanceof stdClass);
var_dump($clone->foo === 'bar');
var_dump($clone->num === 42);
var_dump($clone !== $o);

// ── Nested objects ──
$o = new stdClass();
$o->child = new stdClass();
$o->child->val = 'inner';
$clone = deepclone_from_array(deepclone_to_array($o));
var_dump($clone->child instanceof stdClass);
var_dump($clone->child->val === 'inner');
var_dump($clone->child !== $o->child);

// ── Shared object reference ──
$child = new stdClass();
$child->x = 1;
$o = new stdClass();
$o->a = $child;
$o->b = $child;
$clone = deepclone_from_array(deepclone_to_array($o));
var_dump($clone->a === $clone->b);
var_dump($clone->a !== $child);
var_dump($clone->a->x === 1);

// ── Circular reference ──
$a = new stdClass();
$b = new stdClass();
$a->ref = $b;
$b->ref = $a;
$clone = deepclone_from_array(deepclone_to_array($a));
var_dump($clone->ref->ref === $clone);

// ── Hard references (scalar) ──
$v = [1];
$v[] = &$v[0];
$clone = deepclone_from_array(deepclone_to_array($v));
var_dump($clone[0] === 1);
$clone[0] = 999;
var_dump($clone[1] === 999);

// ── Hard references (recursive) ──
$v = [];
$v[0] = &$v;
$clone = deepclone_from_array(deepclone_to_array($v));
var_dump($clone[0] === $clone);

// ── Named closure (global function) ──
// Serializing/resolving a runtime named callable requires the opt-in on both ends.
try {
    deepclone_to_array(strlen(...));
    echo "no throw\n";
} catch (ValueError $e) {
    var_dump($e->getMessage() === 'deepclone_to_array(): serializing a closure over the named callable "strlen" requires enabling the allow_named_closures option');
}
$clone = deepclone_from_array(deepclone_to_array(strlen(...), allow_named_closures: true), allow_named_closures: true);
var_dump($clone('hello') === 5);

// ── Wide fixture (50 objects) ──
$r = new stdClass();
$r->items = [];
for ($i = 0; $i < 50; $i++) {
    $it = new stdClass();
    $it->id = $i;
    $it->label = "item-$i";
    $r->items[] = $it;
}
$r->meta = new stdClass();
$r->meta->count = 50;
$clone = deepclone_from_array(deepclone_to_array($r));
var_dump(count($clone->items) === 50);
var_dump($clone->items[0]->id === 0);
var_dump($clone->items[49]->label === 'item-49');
var_dump($clone->meta->count === 50);
var_dump($clone !== $r);

// ── COW string preservation ──
$big = str_repeat('x', 10000);
$o = new stdClass();
$o->data = $big;
$clone = deepclone_from_array(deepclone_to_array($o));
var_dump($clone->data === $big);

// ── Multiple consecutive clones are independent ──
$v = [1];
$v[] = &$v[0];
$data = deepclone_to_array($v);
$c1 = deepclone_from_array($data);
$c2 = deepclone_from_array($data);
$c1[0] = 999;
var_dump($c1[1] === 999);
var_dump($c2[0] === 1);
var_dump($c2[1] === 1);

echo "Done\n";
?>
--EXPECTF--
%Abool(true)
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
