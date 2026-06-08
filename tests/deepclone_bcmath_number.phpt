--TEST--
deepclone round-trips BcMath\Number, refuses to hydrate it, and never crashes on an uninitialized one (symfony/symfony#64323)
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (!class_exists('BcMath\Number')) die('skip requires BcMath\Number (PHP 8.4+ with ext/bcmath)'); ?>
--FILE--
<?php
use BcMath\Number;

// ── Round-trip: value, scale, shared identity and nesting are preserved.
//    BcMath\Number is a final internal class with a custom create_object, so
//    newInstanceWithoutConstructor() is rejected and an empty O: unserialize is
//    refused by __unserialize(); the round-trip must replay the full state. ──
$n = new Number('12.34');
$o = (object) ['a' => $n, 'b' => $n, 'list' => [$n, new Number('5')]];
$c = deepclone_from_array(deepclone_to_array($o));

var_dump($c->a instanceof Number);
var_dump((string) $c->a);
var_dump($c->a->scale);
var_dump($c->a !== $n);          // a real clone, not the original
var_dump($c->a === $c->b);       // shared identity preserved
var_dump($c->a === $c->list[0]); // preserved across the whole graph
var_dump((string) $c->list[1]);

// ── A top-level Number round-trips and stays usable. ──
$top = deepclone_from_array(deepclone_to_array(new Number('99.999')));
var_dump((string) $top, $top->scale, (string) $top->add('0.5'));

// ── deepclone_hydrate() injects properties into an empty shell, which cannot
//    produce a valid BcMath\Number; it must refuse rather than yield a broken
//    instance. ──
try {
    deepclone_hydrate(Number::class, ['value' => '7.5']);
} catch (\DeepClone\NotInstantiableException $e) {
    echo $e->getMessage(), "\n";
}

// ── Defensive: an uninitialized instance (forced here via a hand-mutated
//    payload that drops the __unserialize replay) must throw a clean Error on
//    use, never segfault. ──
$d = deepclone_to_array(new Number('1'));
$d['objectMeta'][0][1] = 0; // clear the __unserialize flag
$d['states'] = [];          // and drop the replay
$u = deepclone_from_array($d);
var_dump($u instanceof Number);
foreach (['toString' => fn() => (string) $u, 'clone' => fn() => clone $u, 'add' => fn() => $u->add(1)] as $op => $f) {
    try { $f(); echo "$op: no error\n"; }
    catch (\Error $e) { echo "$op: ", $e->getMessage(), "\n"; }
}
?>
--EXPECT--
bool(true)
string(5) "12.34"
int(2)
bool(true)
bool(true)
bool(true)
string(1) "5"
string(6) "99.999"
int(3)
string(7) "100.499"
Class "BcMath\Number" is not instantiable.
bool(true)
toString: The BcMath\Number object has not been correctly initialized by its constructor
clone: The BcMath\Number object has not been correctly initialized by its constructor
add: The BcMath\Number object has not been correctly initialized by its constructor
