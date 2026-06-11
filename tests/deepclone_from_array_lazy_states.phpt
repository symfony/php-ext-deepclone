--TEST--
deepclone_from_array() defers __wakeup/__unserialize of closure-bearing ghosts
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

function uninit(object $o): bool
{
    return (new ReflectionClass($o))->isUninitializedLazyObject($o);
}

// ── __unserialize node whose state carries a closure: ghost, hook deferred ──
class U {
    public ?Closure $cb = null;
    public string $seen = '';
    public function __serialize(): array { return ['cb' => $this->cb]; }
    public function __unserialize(array $d): void {
        $this->cb = $d['cb'];
        $this->seen = 'ran:' . ($this->cb)('abc');
    }
}
$u = new U; $u->cb = strlen(...);
$copy = deepclone_from_array(deepclone_to_array($u, allow_named_closures: true), allow_named_closures: true);
var_dump(uninit($copy));
var_dump($copy->seen);   // first touch runs __unserialize
var_dump(uninit($copy));

// ── __wakeup node with a closure prop: ghost, wakeup deferred after slots ──
class W {
    public ?Closure $cb = null;
    public string $seen = '';
    public function __wakeup(): void { $this->seen = 'woke:' . ($this->cb)('xy'); }
}
$w = new W; $w->cb = strrev(...);
$copy = deepclone_from_array(deepclone_to_array($w, allow_named_closures: true), allow_named_closures: true);
var_dump(uninit($copy));
var_dump($copy->seen);

// ── An eager __unserialize node touching a deferred-state ghost mid-phase-9
//    gets the fully replayed state, in either "states" entry order ──
class EagerU {
    public mixed $peer = null;
    public string $got = '';
    public function __serialize(): array { return ['peer' => $this->peer]; }
    public function __unserialize(array $d): void {
        $this->peer = $d['peer'];
        $this->got = $this->peer->seen;
    }
}
$g = new U; $g->cb = strlen(...);
$e = new EagerU; $e->peer = $g;
$payload = deepclone_to_array($e, allow_named_closures: true);
foreach ([$payload, ['states' => array_reverse($payload['states'], true)] + $payload] as $p) {
    var_dump(deepclone_from_array($p, allow_named_closures: true)->got);
}

// ── Nested deferral: a deferred-state ghost whose __unserialize touches a
//    peer that is itself a deferred-state ghost, in either entry order ──
class Pair {
    public ?Closure $cb = null;
    public mixed $peer = null;
    public string $seen = '';
    public function __serialize(): array { return ['cb' => $this->cb, 'peer' => $this->peer]; }
    public function __unserialize(array $d): void {
        $this->cb = $d['cb'];
        $this->peer = $d['peer'];
        $this->seen = 'pair:' . ($this->cb)('zz')
            . '/' . ($this->peer instanceof Pair ? ($this->peer->seen ?: 'peer-pending') : 'leaf');
    }
}
$a = new Pair; $b = new Pair;
$a->cb = strlen(...); $b->cb = strrev(...);
$a->peer = $b; $b->peer = 'leaf-b';
$payload = deepclone_to_array($a, allow_named_closures: true);
foreach ([$payload, ['states' => array_reverse($payload['states'], true)] + $payload] as $p) {
    $copy = deepclone_from_array($p, allow_named_closures: true);
    var_dump(uninit($copy));
    var_dump($copy->seen);          // hydrates $a; reading peer->seen nests into $b
    var_dump($copy->peer->seen);
}

// ── Handcrafted: property slots AND an __unserialize state on one ghost id
//    replay slots first, then the state, like the eager phase order ──
class Combo {
    public ?Closure $cb = null;
    public string $seen = '';
    public function __serialize(): array { return ['cb' => $this->cb]; }
    public function __unserialize(array $d): void {
        $this->cb = $d['cb'];
        $this->seen .= 'ran:' . ($this->cb)('abc');
    }
}
$payload = deepclone_to_array((function () { $c = new Combo; $c->cb = strlen(...); return $c; })(), allow_named_closures: true);
$payload['properties'] = ['stdClass' => ['seen' => [0 => 'slot-written+']]];
$copy = deepclone_from_array($payload, allow_named_closures: true);
var_dump(uninit($copy));
var_dump($copy->seen); // slot value first, then the appending hook

// ── Validation of state replays stays eager even for ghosts ──
class V {
    public ?Closure $cb = null;
    public function __serialize(): array { return ['cb' => $this->cb]; }
    public function __unserialize(array $d): void { $this->cb = $d['cb']; }
}
$v = new V; $v->cb = strlen(...);
$payload = deepclone_to_array($v, allow_named_closures: true);

// flagged for replay but no states entry
$broken = $payload;
unset($broken['states']);
try {
    deepclone_from_array($broken, allow_named_closures: true);
    echo "no error?!\n";
} catch (ValueError $err) {
    echo $err->getMessage(), "\n";
}

// same divergence probed with an eager toucher whose hook reaches a
// __wakeup ghost mid-call: the ghost's hook must NOT run when its own
// states entry is missing (eager parity: the call fails either way,
// without the side effect)
class NoisyWakeup {
    public ?Closure $cb = null;
    public function __wakeup(): void { echo "noisy hook ran?!\n"; }
}
class Toucher {
    public mixed $peer = null;
    public function __serialize(): array { return ['peer' => $this->peer]; }
    public function __unserialize(array $d): void {
        $this->peer = $d['peer'];
        $this->peer->cb; // initializes the ghost mid-phase-9
    }
}
$g = new NoisyWakeup; $g->cb = strlen(...);
$t = new Toucher; $t->peer = $g;
$broken = deepclone_to_array($t, allow_named_closures: true);
// drop the ghost's __wakeup entry (the only int entry), keep its flag
$broken['states'] = array_filter($broken['states'], fn ($e) => !is_int($e));
try {
    deepclone_from_array($broken, allow_named_closures: true);
    echo "no error?!\n";
} catch (ValueError $err) {
    echo $err->getMessage(), "\n";
}

// duplicate states entries for the same id
$broken = $payload;
$broken['states'][] = $broken['states'][1];
try {
    deepclone_from_array($broken, allow_named_closures: true);
    echo "no error?!\n";
} catch (ValueError $err) {
    echo $err->getMessage(), "\n";
}

// ── Deferred state errors retry like any ghost: a state closure whose
//    target function disappears throws at first touch, ghost stays lazy ──
function deepmap(array $a): array
{
    $r = [];
    foreach ($a as $k => $v) {
        $r[$k] = is_array($v) ? deepmap($v) : ($v === 'strlen' ? 'no_such_function_xyz' : $v);
    }
    return $r;
}
$ghost = deepclone_from_array(deepmap($payload), allow_named_closures: true);
var_dump(uninit($ghost));
foreach ([1, 2] as $try) {
    try {
        $ghost->cb;
        echo "no error?!\n";
    } catch (ValueError $err) {
        echo "try $try: ", $err->getMessage(), "\n";
    }
    var_dump(uninit($ghost));
}

?>
--EXPECT--
bool(true)
string(5) "ran:3"
bool(false)
bool(true)
string(7) "woke:yx"
string(5) "ran:3"
string(5) "ran:3"
bool(true)
string(19) "pair:2/pair:zz/leaf"
string(12) "pair:zz/leaf"
bool(true)
string(19) "pair:2/pair:zz/leaf"
string(12) "pair:zz/leaf"
bool(true)
string(18) "slot-written+ran:3"
deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 0 flags object for state replay but no matching "states" entry was found
deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 1 flags object for state replay but no matching "states" entry was found
deepclone_from_array(): Argument #1 ($data) "states" has an __unserialize entry for object id 0 but "objectMeta" does not flag it for __unserialize
bool(true)
try 1: deepclone_from_array(): malformed payload, named-closure function or method not found
bool(true)
try 2: deepclone_from_array(): malformed payload, named-closure function or method not found
bool(true)
