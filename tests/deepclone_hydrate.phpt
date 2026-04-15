--TEST--
deepclone_hydrate() instantiates and hydrates objects with scoped properties
--EXTENSIONS--
deepclone
--FILE--
<?php

class Base {
    private string $secret = '';
    public function getSecret(): string { return $this->secret; }
}

class Child extends Base {
    protected int $num = 0;
    public string $pub = '';
    public function getNum(): int { return $this->num; }
}

class Foo {
    protected $prot;
    private $priv;
    public readonly int $ro;
}

#[AllowDynamicProperties]
class Bar extends Foo {
    private $priv;
}

// === Scoped vars (2nd parameter) ===

// Instantiate from class name
$obj = deepclone_hydrate('Child', [
    'Base' => ['secret' => 'hidden'],
    'Child' => ['num' => 42],
    'stdClass' => ['pub' => 'visible'],
]);
var_dump($obj instanceof Child);
var_dump($obj->getSecret() === 'hidden');
var_dump($obj->getNum() === 42);
var_dump($obj->pub === 'visible');

// Hydrate existing object
$existing = new Child();
$result = deepclone_hydrate($existing, [
    'Base' => ['secret' => 'updated'],
    'stdClass' => ['pub' => 'changed'],
]);
var_dump($result === $existing);
var_dump($result->getSecret() === 'updated');
var_dump($result->pub === 'changed');
var_dump($result->getNum() === 0); // untouched

// stdClass
$o = deepclone_hydrate('stdClass', ['stdClass' => ['x' => 1, 'y' => 'hi']]);
var_dump($o->x === 1);
var_dump($o->y === 'hi');

// SplObjectStorage — "\0" key convention
$s = new SplObjectStorage();
$o1 = new stdClass(); $o1->name = 'a';
$o2 = new stdClass(); $o2->name = 'b';
$result = deepclone_hydrate($s, ['stdClass' => ["\0" => [$o1, 'info1', $o2, 'info2']]]);
var_dump($result === $s);
var_dump($result->count() === 2);
$result->rewind();
var_dump($result->current() === $o1);
var_dump($result->getInfo() === 'info1');

// ArrayObject — "\0" key convention
$ao = deepclone_hydrate('ArrayObject', [
    'stdClass' => ["\0" => [['x' => 1, 'y' => 2], ArrayObject::ARRAY_AS_PROPS]],
]);
var_dump($ao instanceof ArrayObject);
var_dump($ao['x'] === 1);
var_dump($ao->getFlags() === ArrayObject::ARRAY_AS_PROPS);

// ArrayIterator — "\0" key convention
$ai = deepclone_hydrate('ArrayIterator', [
    'stdClass' => ["\0" => [['a', 'b', 'c']]],
]);
var_dump($ai instanceof ArrayIterator);
var_dump(count($ai) === 3);

// === Mangled vars (3rd parameter) ===

// stdClass with flat properties
$o = deepclone_hydrate('stdClass', ['p' => 123], DEEPCLONE_HYDRATE_MANGLED_VARS);
var_dump($o->p === 123);

// Case-insensitive class name
$o = deepclone_hydrate('STDcLASS', ['p' => 123], DEEPCLONE_HYDRATE_MANGLED_VARS);
var_dump($o->p === 123);

// ArrayObject via "\0" key in mangled-vars mode
$ao = deepclone_hydrate('ArrayObject', ["\0" => [[123]]], DEEPCLONE_HYDRATE_MANGLED_VARS);
var_dump($ao[0] === 123);

// ArrayObject via "\0" key in scoped mode (own class as scope)
$ao = deepclone_hydrate('ArrayObject', ['ArrayObject' => ["\0" => [[456]]]]);
var_dump($ao[0] === 456);

// SplObjectStorage via "\0" key in mangled-vars mode
$o1 = new stdClass();
$s = deepclone_hydrate('SplObjectStorage', ["\0" => [$o1, 'data']], DEEPCLONE_HYDRATE_MANGLED_VARS);
var_dump($s->count() === 1);

// SplObjectStorage via "\0" key in $scoped_vars
$o1 = new stdClass();
$s = deepclone_hydrate('SplObjectStorage', ['SplObjectStorage' => ["\0" => [$o1, 'info']]]);
var_dump($s->count() === 1);

$expected = [
    "\0*\0prot" => 345,
    "\0Bar\0priv" => 123,
    "\0Foo\0priv" => 234,
    'dyn' => 456,
    'ro' => 567,
];

// Mangled key format
$actual = (array) deepclone_hydrate('Bar', [
    "\0*\0prot" => 345,
    "\0Bar\0priv" => 123,
    "\0Foo\0priv" => 234,
    'dyn' => 456,
    'ro' => 567,
], DEEPCLONE_HYDRATE_MANGLED_VARS);
ksort($actual);
var_dump($actual === $expected);

// Exception trace (internal class hydration)
$e = deepclone_hydrate('Exception', ['trace' => [234]], DEEPCLONE_HYDRATE_MANGLED_VARS);
var_dump($e->getTrace() === [234]);

// Readonly: hydrating an already-initialized readonly property throws,
// matching ReflectionProperty::setRawValue semantics (engine checks
// ZEND_ACC_READONLY during the write).
class ReadonlyClass {
    public string $status = 'new';
    private readonly int $value;
    public function __construct(int $value) { $this->value = $value; }
    public function getValue(): int { return $this->value; }
}
$obj = new ReadonlyClass(123);
try {
    deepclone_hydrate($obj, ['value' => 456], DEEPCLONE_HYDRATE_MANGLED_VARS);
    var_dump(false);
} catch (\Error $e) {
    var_dump(str_contains($e->getMessage(), 'readonly'));
}
var_dump($obj->getValue() === 123);

// Readonly: uninitialized → sets value
$obj = deepclone_hydrate('ReadonlyClass', ['ReadonlyClass' => ['value' => 456]]);
var_dump($obj->getValue() === 456);

// PHP references preservation (mangled mode keeps refs across the hydrate)
$a = 1;
$props = ['p1' => &$a, 'p2' => &$a];
$obj = deepclone_hydrate('stdClass', $props, DEEPCLONE_HYDRATE_MANGLED_VARS);
var_dump($obj->p1 === 1 && $obj->p2 === 1);
$a = 2;
var_dump($obj->p1 === 2 && $obj->p2 === 2);

// References in scoped properties
$v = 'hello';
$obj = deepclone_hydrate('stdClass', ['stdClass' => ['x' => &$v, 'y' => &$v]]);
$v = 'world';
var_dump($obj->x === 'world' && $obj->y === 'world');

// === Grandparent private (3-level inheritance) ===
class GP { private string $secret = ''; public function getSecret(): string { return $this->secret; } }
class P extends GP { private int $mid = 0; public function getMid(): int { return $this->mid; } }
class C extends P { public string $pub = ''; }

$o = deepclone_hydrate('C', ["\0GP\0secret" => 'gp_val', "\0P\0mid" => 42, 'pub' => 'hi'], DEEPCLONE_HYDRATE_MANGLED_VARS);
var_dump($o->getSecret() === 'gp_val');
var_dump($o->getMid() === 42);
var_dump($o->pub === 'hi');

// === Error cases ===

// Unknown class
try {
    deepclone_hydrate('NoSuchClass99', []);
} catch (\DeepClone\ClassNotFoundException $e) {
    var_dump(str_contains($e->getMessage(), 'NoSuchClass99'));
}

// Abstract class
try {
    deepclone_hydrate('SplHeap', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'SplHeap'));
}

// Interface
try {
    deepclone_hydrate('Throwable', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'Throwable'));
}

// Enum
enum HydrateColor { case Red; }
try {
    deepclone_hydrate('HydrateColor', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'HydrateColor'));
}

// Trait
trait HydrateTrait {}
try {
    deepclone_hydrate('HydrateTrait', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'HydrateTrait'));
}

// Reflector subclass (internal, cannot function without constructor)
try {
    deepclone_hydrate('ReflectionClass', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'ReflectionClass'));
}

// Closure (internal final with create_object)
try {
    deepclone_hydrate('Closure', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'Closure'));
}

// SplFileInfo (internal, serialize fails)
try {
    deepclone_hydrate('SplFileInfo', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'SplFileInfo'));
}

// Bad type
try {
    deepclone_hydrate([], []);
} catch (\TypeError $e) {
    var_dump(str_contains($e->getMessage(), 'must be of type object|string'));
}

// Integer key in $mangled_vars → ValueError
try {
    deepclone_hydrate('stdClass', [0 => 'val'], DEEPCLONE_HYDRATE_MANGLED_VARS);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'string keys'));
}

// Non-array in $scoped_vars → ValueError
try {
    deepclone_hydrate('stdClass', ['stdClass' => 'not-array']);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'array values'));
}

// NUL byte in property name inside scoped array → ValueError
try {
    deepclone_hydrate('stdClass', ['stdClass' => ["foo\0bar" => 'val']]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'invalid property name'));
}

// NUL byte in mangled key property portion → ValueError
try {
    deepclone_hydrate('stdClass', ["\0*\0foo\0bar" => 'val'], DEEPCLONE_HYDRATE_MANGLED_VARS);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'invalid mangled key'));
}

// Integer key inside scoped array → ValueError
try {
    deepclone_hydrate('stdClass', ['stdClass' => [0 => 'val']]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'string keys'));
}

// Mangled key inside scoped array → ValueError
try {
    deepclone_hydrate('stdClass', ['stdClass' => ["\0stdClass\0x" => 'val']]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'mangled key'));
}

// Interface as scope → ValueError
interface HydrateIface {}
class HydrateImpl implements HydrateIface { public string $x = ''; }
try {
    deepclone_hydrate('HydrateImpl', ['HydrateIface' => ['x' => 'hello']]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'not a parent'));
}

// Unrelated scope → ValueError
try {
    deepclone_hydrate('stdClass', ['SplHeap' => ['x' => 1]]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'not a parent'));
}

// Non-existing scope → ValueError (no autoloader triggered)
try {
    deepclone_hydrate('stdClass', ['NonExistent' => ['x' => 1]]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'not a parent'));
}

// No arguments (just instantiate)
$o = deepclone_hydrate('stdClass');
var_dump($o instanceof stdClass);

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
