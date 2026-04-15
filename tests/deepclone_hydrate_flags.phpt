--TEST--
deepclone_hydrate() $flags parameter: CALL_HOOKS / validation / lazy semantics
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) {
    die('skip property hooks / lazy objects require PHP 8.4+');
}
?>
--FILE--
<?php

class HookedBacking {
    public int $x = 0 {
        set(int $v) { $this->x = $v * 10; }
    }
}

// Default (flags=0) → setRawValue: bypass hook
$o = deepclone_hydrate(HookedBacking::class, [HookedBacking::class => ['x' => 7]]);
var_dump($o->x === 7);

// DEEPCLONE_HYDRATE_CALL_HOOKS → setValue: invoke hook
$o = deepclone_hydrate(HookedBacking::class, [HookedBacking::class => ['x' => 7]], [], DEEPCLONE_HYDRATE_CALL_HOOKS);
var_dump($o->x === 70);

// Unknown bit → ValueError
try {
    deepclone_hydrate(HookedBacking::class, [], [], 1 << 30);
    var_dump(false);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'unknown bits'));
}

// Mutually exclusive flags → ValueError
try {
    deepclone_hydrate(HookedBacking::class, [], [], DEEPCLONE_HYDRATE_CALL_HOOKS | DEEPCLONE_HYDRATE_NO_LAZY_INIT);
    var_dump(false);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'mutually exclusive'));
}

// Lazy ghost, default flag: hydrate triggers the lazy initializer on first
// access (matching ReflectionProperty::setValue / setRawValue). The hydrate
// value wins for the written prop; initializer values survive for the rest.
class Lazy { public int $a = 0; public string $b = ''; }
$rc = new ReflectionClass(Lazy::class);

$initRan = 0;
$ghost = $rc->newLazyGhost(function (Lazy $o) use (&$initRan) {
    ++$initRan;
    $o->a = 1;
    $o->b = 'init';
});
deepclone_hydrate($ghost, [Lazy::class => ['a' => 99]]);
var_dump($initRan === 1);        // initializer ran once
var_dump($ghost->a === 99);      // hydrate value wins
var_dump($ghost->b === 'init');  // initializer value preserved

// Lazy ghost, NO_LAZY_INIT: hydrate skips the initializer. After writing
// the single lazy prop, the object is realized (no more lazy props).
$initRan = 0;
$ghost = $rc->newLazyGhost(function (Lazy $o) use (&$initRan) {
    ++$initRan;
    $o->a = 1;
    $o->b = 'init';
});
deepclone_hydrate($ghost, [Lazy::class => ['a' => 99, 'b' => 'hyd']], [], DEEPCLONE_HYDRATE_NO_LAZY_INIT);
var_dump($initRan === 0);        // initializer did NOT run
var_dump($ghost->a === 99);
var_dump($ghost->b === 'hyd');
var_dump($initRan === 0);        // subsequent read still doesn't init (realized)

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
Done
