--TEST--
deepclone_hydrate() instantiates and hydrates objects from a flat mangled-key array
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

// === Basic hydration — $vars is the flat (array) $obj shape ===

// Instantiate from class name — mix of private (mangled), protected (bare or
// "\0*\0name"), and public (bare) keys.
$obj = deepclone_hydrate('Child', [
    "\0Base\0secret" => 'hidden',
    'num' => 42,
    'pub' => 'visible',
]);
var_dump($obj instanceof Child);
var_dump($obj->getSecret() === 'hidden');
var_dump($obj->getNum() === 42);
var_dump($obj->pub === 'visible');

// Hydrate existing object — partial update keeps untouched props
$existing = new Child();
$result = deepclone_hydrate($existing, [
    "\0Base\0secret" => 'updated',
    'pub' => 'changed',
]);
var_dump($result === $existing);
var_dump($result->getSecret() === 'updated');
var_dump($result->pub === 'changed');
var_dump($result->getNum() === 0); // untouched

// stdClass with dynamic properties
$o = deepclone_hydrate('stdClass', ['x' => 1, 'y' => 'hi']);
var_dump($o->x === 1);
var_dump($o->y === 'hi');

// Case-insensitive class name
$o = deepclone_hydrate('STDcLASS', ['p' => 123]);
var_dump($o->p === 123);

// === SPL classes (ArrayObject, ArrayIterator, SplObjectStorage) ===
// These ship __serialize/__unserialize since PHP 7.4. deepclone_hydrate()
// instantiates them; callers wire up state by calling __unserialize() with
// the array shape these classes document, or by going through
// deepclone_from_array() which handles it natively.

// ArrayObject + __unserialize
$ao = deepclone_hydrate('ArrayObject');
$ao->__unserialize([ArrayObject::ARRAY_AS_PROPS, ['x' => 1, 'y' => 2], []]);
var_dump($ao instanceof ArrayObject);
var_dump($ao['x'] === 1);
var_dump($ao->getFlags() === ArrayObject::ARRAY_AS_PROPS);

// ArrayIterator + __unserialize
$ai = deepclone_hydrate('ArrayIterator');
$ai->__unserialize([0, ['a', 'b', 'c'], []]);
var_dump($ai instanceof ArrayIterator);
var_dump(count($ai) === 3);

// SplObjectStorage + __unserialize
$o1 = new stdClass(); $o2 = new stdClass();
$s = deepclone_hydrate('SplObjectStorage');
$s->__unserialize([[$o1, 'info1', $o2, 'info2'], []]);
var_dump($s->count() === 2);

// === (array) $obj round-trip — all four mangled-key shapes ===

$expected = [
    "\0*\0prot" => 345,
    "\0Bar\0priv" => 123,
    "\0Foo\0priv" => 234,
    'dyn' => 456,
    'ro' => 567,
];

$actual = (array) deepclone_hydrate('Bar', [
    "\0*\0prot" => 345,
    "\0Bar\0priv" => 123,
    "\0Foo\0priv" => 234,
    'dyn' => 456,
    'ro' => 567,
]);
ksort($actual);
var_dump($actual === $expected);

// Same round trip with every mangled key resolving to the object's own class
// scope, with no parent-scoped key to hint that the keys need re-keying.
$src = new Foo();
(function () { $this->prot = 345; $this->priv = 123; })->call($src);
$vars = (array) $src;
var_dump((array) deepclone_hydrate('Foo', $vars) === $vars);

// Exception trace (internal class hydration)
$e = deepclone_hydrate('Exception', ['trace' => [234]]);
var_dump($e->getTrace() === [234]);

// === Readonly ===

// Rehydrating an already-initialized readonly property with a DIFFERENT value
// throws — engine enforces ZEND_ACC_READONLY.
class ReadonlyClass {
    public string $status = 'new';
    private readonly int $value;
    public function __construct(int $value) { $this->value = $value; }
    public function getValue(): int { return $this->value; }
}
$obj = new ReadonlyClass(123);
try {
    deepclone_hydrate($obj, ['value' => 456]);
    var_dump(false);
} catch (\Error $e) {
    var_dump(str_contains($e->getMessage(), 'readonly'));
}
var_dump($obj->getValue() === 123);

// Uninitialized readonly → sets value (first write wins)
$obj = deepclone_hydrate('ReadonlyClass', ['value' => 456]);
var_dump($obj->getValue() === 456);

// === PHP & references ===

// Default: references are dropped on write
$a = 1;
$props = ['p1' => &$a, 'p2' => &$a];
$obj = deepclone_hydrate('stdClass', $props);
var_dump($obj->p1 === 1 && $obj->p2 === 1);
$a = 2;
var_dump($obj->p1 === 1 && $obj->p2 === 1);

// PRESERVE_REFS: the ref link is kept across the hydrate
$a = 1;
$props = ['p1' => &$a, 'p2' => &$a];
$obj = deepclone_hydrate('stdClass', $props, DEEPCLONE_HYDRATE_PRESERVE_REFS);
var_dump($obj->p1 === 1 && $obj->p2 === 1);
$a = 2;
var_dump($obj->p1 === 2 && $obj->p2 === 2);

// === Grandparent private (3-level inheritance) ===

class GP { private string $secret = ''; public function getSecret(): string { return $this->secret; } }
class P extends GP { private int $mid = 0; public function getMid(): int { return $this->mid; } }
class C extends P { public string $pub = ''; }

$o = deepclone_hydrate('C', [
    "\0GP\0secret" => 'gp_val',
    "\0P\0mid" => 42,
    'pub' => 'hi',
]);
var_dump($o->getSecret() === 'gp_val');
var_dump($o->getMid() === 42);
var_dump($o->pub === 'hi');

// Bare name reaches a parent-private slot when it's the only one of that name
// in the inheritance chain — no mangled key required.
$o = deepclone_hydrate('C', [
    'secret' => 'bare_gp',
    'mid' => 77,
    'pub' => 'bare',
]);
var_dump($o->getSecret() === 'bare_gp');
var_dump($o->getMid() === 77);
var_dump($o->pub === 'bare');

$props = (array) $o;
var_dump(!isset($props['secret']));                    // no stray dynamic prop
var_dump(!isset($props['mid']));                       //        ,,
var_dump(isset($props["\0GP\0secret"]));               // went to GP's slot
var_dump(isset($props["\0P\0mid"]));                   // went to P's slot

// === Error cases ===

try {
    deepclone_hydrate('NoSuchClass99', []);
} catch (\DeepClone\ClassNotFoundException $e) {
    var_dump(str_contains($e->getMessage(), 'NoSuchClass99'));
}

try {
    deepclone_hydrate('SplHeap', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'SplHeap'));
}

try {
    deepclone_hydrate('Throwable', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'Throwable'));
}

enum HydrateColor { case Red; }
try {
    deepclone_hydrate('HydrateColor', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'HydrateColor'));
}

trait HydrateTrait {}
try {
    deepclone_hydrate('HydrateTrait', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'HydrateTrait'));
}

try {
    deepclone_hydrate('ReflectionClass', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'ReflectionClass'));
}

try {
    deepclone_hydrate('Closure', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'Closure'));
}

try {
    deepclone_hydrate('SplFileInfo', []);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'SplFileInfo'));
}

try {
    deepclone_hydrate([], []);
} catch (\TypeError $e) {
    var_dump(str_contains($e->getMessage(), 'must be of type object|string'));
}

// Malformed mangled key — missing second NUL
try {
    deepclone_hydrate('stdClass', ["\0broken" => 'val']);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'invalid mangled key'));
}

// Malformed mangled key — empty class name
try {
    deepclone_hydrate('stdClass', ["\0\0prop" => 'val']);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'invalid mangled key'));
}

// Embedded NUL in the property name portion of a mangled key
try {
    deepclone_hydrate('stdClass', ["\0*\0foo\0bar" => 'val']);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'invalid mangled key'));
}

// Mangled-private key referencing a non-parent class → ValueError
class HydrateUnrelated { public int $x = 0; }
try {
    deepclone_hydrate('stdClass', ["\0HydrateUnrelated\0x" => 1]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'not a parent'));
}

// Mangled-protected key referencing an undeclared property → ValueError
// (a "\0*\0name" form specifically targets a protected slot; if the slot
// doesn't exist, reject instead of silently creating a dynamic property).
class HydrateProt { protected int $real = 0; }
try {
    deepclone_hydrate('HydrateProt', ["\0*\0undeclared" => 1]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'does not declare'));
}

// Mangled-private key with a valid parent class but undeclared property → ValueError
class HydrateChildProt extends HydrateProt {}
try {
    deepclone_hydrate('HydrateChildProt', ["\0HydrateProt\0undeclared" => 1]);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'does not declare'));
}

// Mangled-private key referencing an interface (which isn't a parent class)
interface HydrateIface {}
class HydrateImpl implements HydrateIface { public string $x = ''; }
try {
    deepclone_hydrate('HydrateImpl', ["\0HydrateIface\0x" => 'hello']);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'not a parent'));
}

// Integer keys coerce to string (matches unserialize())
$o = deepclone_hydrate('stdClass', [0 => 'val']);
var_dump($o->{'0'} === 'val');

// NUL-in-middle of a bare property name: stored as a raw dynamic property
// on stdClass (matches unserialize()).
$o = deepclone_hydrate('stdClass', ["foo\0bar" => 'val']);
var_dump(((array) $o)["foo\0bar"] === 'val');

// No $vars (just instantiate)
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
bool(true)
Done
