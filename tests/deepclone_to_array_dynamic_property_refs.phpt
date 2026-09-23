--TEST--
deepclone_to_array() keeps references on dynamic properties
--EXTENSIONS--
deepclone
--FILE--
<?php

#[AllowDynamicProperties]
class DynamicRefs
{
    public $declared;
    public $cb;
}

function bound(object $o, string $from, string $to): bool
{
    $o->$from = 42;

    return 42 === $o->$to;
}

// Between dynamic properties, and between a declared and a dynamic one
$o = new DynamicRefs();
$o->a = 1;
$o->b = &$o->a;
$o->declared = 2;
$o->c = &$o->declared;
$c = deepclone_from_array(deepclone_to_array($o));
var_dump(bound($c, 'a', 'b'), bound($c, 'declared', 'c'));

// Across objects and array elements
$o1 = new DynamicRefs();
$o2 = new DynamicRefs();
$o2->y = 1;
$o1->x = &$o2->y;
$arr = [&$o2->y];
[$c1, $c2, $carr] = deepclone_from_array(deepclone_to_array([$o1, $o2, $arr]));
$c1->x = 42;
var_dump($c2->y, $carr[0]);

// On a node that deepclone_from_array() creates as a lazy ghost (PHP 8.4+)
$o = new DynamicRefs();
$o->cb = strlen(...);
$o->a = 1;
$o->b = &$o->a;
$c = deepclone_from_array(deepclone_to_array($o, allow_named_closures: true), allow_named_closures: true);
var_dump(bound($c, 'a', 'b'), ($c->cb)('abc'));
?>
--EXPECT--
bool(true)
bool(true)
int(42)
int(42)
bool(true)
int(3)
