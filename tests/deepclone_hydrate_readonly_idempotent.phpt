--TEST--
deepclone_hydrate() skips readonly writes when the new value is identical (===)
--EXTENSIONS--
deepclone
--FILE--
<?php

class ReadonlyInt
{
    public readonly int $v;
    public function __construct(int $v) { $this->v = $v; }
}

class ReadonlyObj
{
    public readonly \stdClass $o;
    public function __construct(\stdClass $o) { $this->o = $o; }
}

// Same value (===) → silently skipped
$o = new ReadonlyInt(42);
$o = deepclone_hydrate($o, [ReadonlyInt::class => ['v' => 42]]);
var_dump($o->v === 42);

// Different value → engine throws
$o = new ReadonlyInt(42);
try {
    deepclone_hydrate($o, [ReadonlyInt::class => ['v' => 43]]);
    var_dump(false);
} catch (\Error $e) {
    var_dump(str_contains($e->getMessage(), 'readonly'));
}
var_dump($o->v === 42);

// Object identity (same zval) → skipped
$inner = new \stdClass();
$o = new ReadonlyObj($inner);
$o = deepclone_hydrate($o, [ReadonlyObj::class => ['o' => $inner]]);
var_dump($o->o === $inner);

// Different object (even if equal) → engine throws
$o = new ReadonlyObj(new \stdClass());
try {
    deepclone_hydrate($o, [ReadonlyObj::class => ['o' => new \stdClass()]]);
    var_dump(false);
} catch (\Error $e) {
    var_dump(str_contains($e->getMessage(), 'readonly'));
}

// Uninitialized readonly → writes normally
$o = deepclone_hydrate(ReadonlyInt::class, [ReadonlyInt::class => ['v' => 99]]);
var_dump($o->v === 99);

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
