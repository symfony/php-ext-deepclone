--TEST--
deepclone_hydrate() unsets non-nullable typed properties when given null
--EXTENSIONS--
deepclone
--FILE--
<?php

class Typed
{
    public int $x = 42;
    public ?int $y = 7;
    public mixed $m = 'foo';
}

// null into non-nullable typed → slot becomes uninitialized
$o = new Typed();
$o = deepclone_hydrate($o, [Typed::class => ['x' => null]]);
var_dump((new ReflectionProperty(Typed::class, 'x'))->isInitialized($o));

// Reading an uninitialized typed prop throws the standard engine error
try {
    $v = $o->x;
    var_dump(false);
} catch (\Error $e) {
    var_dump(str_contains($e->getMessage(), 'must not be accessed before initialization'));
}

// null into nullable → stored as null
$o = new Typed();
$o = deepclone_hydrate($o, [Typed::class => ['y' => null]]);
var_dump($o->y === null);

// null into mixed → stored as null
$o = new Typed();
$o = deepclone_hydrate($o, [Typed::class => ['m' => null]]);
var_dump($o->m === null);

// CALL_HOOKS is per-prop: a non-hooked typed prop still gets unset, even
// when the flag is set (no set hook on this property to defer to).
$o = new Typed();
$o = deepclone_hydrate($o, [Typed::class => ['x' => null]], [], DEEPCLONE_HYDRATE_CALL_HOOKS);
var_dump((new ReflectionProperty(Typed::class, 'x'))->isInitialized($o));

echo "Done\n";
?>
--EXPECT--
bool(false)
bool(true)
bool(true)
bool(true)
bool(false)
Done
