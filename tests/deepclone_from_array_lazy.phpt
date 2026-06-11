--TEST--
deepclone_from_array() creates native lazy ghosts for closure-bearing nodes
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) {
    die('skip native lazy objects require PHP 8.4+');
}
?>
--FILE--
<?php

class Inner
{
    public function __construct(public int $n = 0, public ?Closure $cb = null) {}
}

class Outer
{
    private string $name = '';
    protected ?Inner $inner = null;
    public ?Outer $self = null;
    public readonly int $ro;
    public ?Closure $fn = null;

    public function __construct(string $name, Inner $i)
    {
        $this->name = $name;
        $this->inner = $i;
        $this->self = $this;
        $this->ro = 7;
        $this->fn = strtoupper(...);
    }

    public function getName(): string { return $this->name; }
    public function getInner(): Inner { return $this->inner; }
}

function uninit(object $o): bool
{
    return (new ReflectionClass($o))->isUninitializedLazyObject($o);
}

$payload = deepclone_to_array(new Outer('hello', new Inner(42, strlen(...))));

// ── Root comes back as an uninitialized ghost ──
$copy = deepclone_from_array($payload);
var_dump(uninit($copy));

// ── var_dump does not initialize ──
var_dump($copy);
var_dump(uninit($copy));

// ── Identity: back-reference to the root is the same object, and the
//    property read initializes the ghost ──
var_dump($copy === $copy->self);
var_dump(uninit($copy));

// ── Private / protected / readonly / closure props hydrated correctly ──
var_dump($copy->getName());
var_dump($copy->ro);
var_dump(($copy->fn)('up'));

// ── Per-node granularity: the child is still lazy after the root hydrated ──
var_dump(uninit($copy->getInner()));
var_dump($copy->getInner()->n);
var_dump(uninit($copy->getInner()));

// ── getLazyInitializer() returns a Closure over the shared context;
//    calling it hydrates ──
$lazy = deepclone_from_array($payload);
$init = (new ReflectionClass(Outer::class))->getLazyInitializer($lazy);
var_dump($init instanceof Closure);
$init($lazy);
var_dump(uninit($lazy), $lazy->getName());

// ── The Closure is created once per call: identical across all ghosts of
//    one call, distinct across calls ──
$lazy = deepclone_from_array($payload);
$initOuter = (new ReflectionClass(Outer::class))->getLazyInitializer($lazy);
$initInner = (new ReflectionClass(Inner::class))->getLazyInitializer($lazy->getInner());
var_dump($initOuter === $initInner);
var_dump($initOuter === (new ReflectionClass(Outer::class))->getLazyInitializer(deepclone_from_array($payload)));

// ── Nodes without closure markers gain nothing from deferral: they always
//    hydrate eagerly (plain value slots are cheaper to hydrate than to
//    ghost) ──
class ScalarOnly { public int $s = 0; public string $t = ''; }
$s = new ScalarOnly; $s->s = 5; $s->t = 'x';
$s2 = deepclone_from_array(deepclone_to_array([$s, new Inner(3, trim(...))]));
var_dump(uninit($s2[0]), $s2[0]->s);
var_dump(uninit($s2[1]), $s2[1]->n);

// ── stdClass-only graphs degrade to plain eager hydration ──
$o = new stdClass;
$o->x = new stdClass;
$o->x->y = 1;
$o2 = deepclone_from_array(deepclone_to_array($o));
var_dump($o2->x->y);

// ── clone initializes the ghost and produces a hydrated copy ──
$lazy = deepclone_from_array($payload);
$clone = clone $lazy;
var_dump(uninit($lazy));
var_dump($clone->getName(), ($clone->fn)('ok'));

?>
--EXPECTF--
bool(true)
lazy ghost object(Outer)#%d (0) {
  ["name":"Outer":private]=>
  uninitialized(string)
  ["inner":protected]=>
  uninitialized(?Inner)
  ["self"]=>
  uninitialized(?Outer)
  ["ro"]=>
  uninitialized(int)
  ["fn"]=>
  uninitialized(?Closure)
}
bool(true)
bool(true)
bool(false)
string(5) "hello"
int(7)
string(2) "UP"
bool(true)
int(42)
bool(false)
bool(true)
bool(false)
string(5) "hello"
bool(true)
bool(false)
bool(false)
int(5)
bool(true)
int(3)
int(1)
bool(false)
string(5) "hello"
string(2) "OK"
