--TEST--
Regressions for bugs surfaced by the libFuzzer harnesses (v0.5.1 fixes)
--EXTENSIONS--
deepclone
--FILE--
<?php

// #1 — DoS via oversized IS_LONG objectMeta (commit 4a0f594).
// A tiny payload with `objectMeta` = a huge integer used to drive
// multi-GB allocations. The reader caps the IS_LONG form at 1M.
try {
    deepclone_from_array([
        'classes' => 'stdClass',
        'objectMeta' => 0x40000000,  // 1G — well over the 1M cap
        'prepared' => 0,
    ]);
    var_dump(false);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'out of range'));
}

// #2 — UAF on ref tree_pos across packed→hash conversion (commit 3e0bf4f).
// Build an array that triggers packed-to-hash mid-copy while holding saved
// tree_pos pointers from an earlier (ref) iteration. Under ASAN this
// would fire a heap-use-after-free; without ASAN it could segfault or
// silently corrupt. The fix forces mixed mode up front.
$ref = 42;
$mixed = [
    0        => &$ref,      // integer key with reference (saves tree_pos)
    'trigger' => 'str',     // string key triggers packed→hash rehash
];
$d = deepclone_from_array(deepclone_to_array($mixed));
var_dump(is_array($d));
var_dump($d[0] === 42);
var_dump($d['trigger'] === 'str');

// #3 — Unsound refcount==1 pool-skip on shared parent (commit 0bc6dc3).
// An object with refcount==1 can be reached twice when the containing
// array has refcount > 1 (shared via multiple slots). Old code would
// skip the pool lookup on the second visit and trip add_new()'s
// "slot already occupied" assertion in debug builds; in release, it
// would silently emit a duplicate objectMeta entry.
// The contained object must have refcount=1 (only reachable via the shared
// HT slot), so bypass the local variable and put an anonymous object in.
$mk = static fn() => (function() { $o = new stdClass(); $o->val = 42; return $o; })();
$shared = [$mk()];           // inner stdClass refcount 1, $shared refcount 1
$graph  = [$shared, $shared]; // $shared HT refcount 2; inner obj refcount still 1

$d = deepclone_to_array($graph);
$c = deepclone_from_array($d);
var_dump(is_array($c));
var_dump($c[0][0]->val === 42);
var_dump($c[1][0]->val === 42);
// Both copies should resolve to the same reconstructed object (object identity
// is preserved within a single deepclone).
var_dump($c[0][0] === $c[1][0]);

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
Done
