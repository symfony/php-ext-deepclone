--TEST--
deepclone_to_array() keeps references between declared properties and unwraps unshared ones
--EXTENSIONS--
deepclone
--FILE--
<?php

#[AllowDynamicProperties]
class Holder
{
    public $a;
    public $b;
    public $other;
}

function bound(object $o): bool
{
    $o->a = 42;

    return 42 === $o->b;
}

// References between declared properties survive, also once the property
// table has been built (here by foreach) or with dynamic properties around
$o = new Holder();
$o->a = 1;
$o->b = &$o->a;
var_dump(bound(deepclone_from_array(deepclone_to_array($o))));

foreach ($o as $v) {
}
var_dump(bound(deepclone_from_array(deepclone_to_array($o))));

$o = new Holder();
$o->a = 1;
$o->b = &$o->a;
$o->dyn = 2;
var_dump(bound(deepclone_from_array(deepclone_to_array($o))));

// A property bound by reference to a variable outside the graph is exported
// as a plain value, including when a later property holds a refcounted value
$o = new Holder();
$o->a = 1;
$x = &$o->a;
$o->other = [str_repeat('x', 3)];
$p = deepclone_to_array($o);
var_dump($p['properties']['stdClass']['a'][0], isset($p['refs']));
$c = deepclone_from_array($p);
var_dump($c->a, $c->other);
unset($p, $c);
var_dump($o->other);

// Same with an object value, and on stdClass with a numeric property name
$o = new Holder();
$o->a = new stdClass();
$x = &$o->a;
$o->other = [str_repeat('y', 3)];
$c = deepclone_from_array(deepclone_to_array($o));
var_dump($c->a instanceof stdClass, $c->other);

$o = new stdClass();
$o->{'1'} = 'one';
$x = &$o->{'1'};
$o->list = [str_repeat('z', 3)];
$c = deepclone_from_array(deepclone_to_array($o));
var_dump($c->{'1'}, $c->list);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
int(1)
bool(false)
int(1)
array(1) {
  [0]=>
  string(3) "xxx"
}
array(1) {
  [0]=>
  string(3) "xxx"
}
bool(true)
array(1) {
  [0]=>
  string(3) "yyy"
}
string(3) "one"
array(1) {
  [0]=>
  string(3) "zzz"
}
