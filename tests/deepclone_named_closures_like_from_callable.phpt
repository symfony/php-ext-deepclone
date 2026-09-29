--TEST--
deepclone_from_array() creates named closures like Closure::fromCallable() does
--EXTENSIONS--
deepclone
--FILE--
<?php

class Base
{
    private $secret = 'base';

    public static function create() { return static::class; }
    public function reveal() { return $this->secret; }
    protected function prot() { return static::class; }
    public function getProt() { return $this->prot(...); }
}

class Child extends Base
{
}

class Other
{
}

function roundtrip($value)
{
    return deepclone_from_array(deepclone_to_array($value, null, true), null, true);
}

// Called on the child class, with the scope of the declaring one
var_dump(roundtrip(Child::create(...))());
var_dump(roundtrip((new Child())->reveal(...))());
var_dump(roundtrip((new Child())->getProt())());

$r = new ReflectionFunction(roundtrip((new Child())->reveal(...)));
var_dump($r->getClosureScopeClass()->name, $r->getClosureCalledClass()->name);

// Methods that __call() and __callStatic() handle
class Magic
{
    public function __call($name, $args) { return "call $name"; }
    public static function __callStatic($name, $args) { return "static $name ".static::class; }
}

class ChildMagic extends Magic
{
}

var_dump(roundtrip((new Magic())->foo(...))());
var_dump(roundtrip(ChildMagic::bar(...))());

$payloads = [
    'top-level function not found' => ['classes' => '', 'objectMeta' => 0, 'prepared' => [null, 'no_such_function'], 'mask' => 0],
    'non-static method without object' => ['classes' => '', 'objectMeta' => 0, 'prepared' => ['Base', 'reveal'], 'mask' => 0],
    'method on an unrelated object' => ['classes' => 'Other', 'objectMeta' => 1, 'prepared' => [[[0, 'prot'], 'Base', 'prot']], 'mask' => [0]],
    'method on an unrelated class' => ['classes' => '', 'objectMeta' => 0, 'prepared' => [[['Other', 'create'], 'Base', 'create']], 'mask' => [0]],
];
foreach ($payloads as $label => $payload) {
    try {
        deepclone_from_array($payload, null, true);
        echo "$label: accepted\n";
    } catch (ValueError $e) {
        echo "$label: ", $e->getMessage(), "\n";
    }
}

// Their shape is checked right away, even when the objects holding them are created lazily
class Holder
{
    public $f;
}

try {
    deepclone_from_array(['classes' => 'Holder', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ['f' => [5]]], 'resolve' => ['stdClass' => ['f' => [0]]]], null, true);
    echo "accepted\n";
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
string(5) "Child"
string(4) "base"
string(5) "Child"
string(4) "Base"
string(5) "Child"
string(8) "call foo"
string(21) "static bar ChildMagic"
top-level function not found: deepclone_from_array(): malformed payload, named-closure function or method not found
non-static method without object: deepclone_from_array(): malformed payload, named-closure method Base::reveal() is not static
method on an unrelated object: deepclone_from_array(): malformed payload, named-closure method Base::prot() cannot be called on Other
method on an unrelated class: deepclone_from_array(): malformed payload, named-closure method Base::create() cannot be called on Other
deepclone_from_array(): malformed payload, named-closure value must be of type array, int given
