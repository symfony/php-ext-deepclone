--TEST--
deepclone_from_array() lazy hydration: abandoned graphs are GC-collectable, closure-free wakeup nodes stay eager
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

class Node { public mixed $v = null; public ?Node $peer = null; public ?Closure $cb = null; }

function build(): array
{
    $a = new Node; $b = new Node;
    $a->cb = strlen(...); $b->cb = strrev(...);   // makes both nodes ghosts
    $a->peer = $b; $b->peer = $a;
    $a->v = 'av'; $b->v = 'bv';
    return deepclone_to_array($a, allow_named_closures: true);
}

// ── A half-lazy graph that goes out of scope is a context↔ghost cycle;
//    the GC must reclaim it ──
$weak = null;
(function () use (&$weak) {
    $a = deepclone_from_array(build(), allow_named_closures: true);
    $weak = WeakReference::create($a);
    $a->v; // hydrate the root, leave the peer lazy
})();
gc_collect_cycles();
var_dump($weak->get());

// ── Fully-lazy abandoned graph too ──
(function () use (&$weak) {
    $a = deepclone_from_array(build(), allow_named_closures: true);
    $weak = WeakReference::create($a);
})();
gc_collect_cycles();
var_dump($weak->get());

// ── Destructors of never-initialized ghosts are skipped (native lazy-object
//    semantics); initialized ones run theirs ──
class Dtor {
    public int $x = 0;
    public ?Closure $cb = null;
    public function __destruct() { echo "dtor {$this->x}\n"; }
}
$src = [new Dtor, new Dtor];
$src[0]->x = 1; $src[0]->cb = strlen(...);
$src[1]->x = 2; $src[1]->cb = strrev(...);
$payload = deepclone_to_array($src, allow_named_closures: true);
unset($src); // "dtor 1", "dtor 2": the sources go away
(function () use ($payload) {
    [$a, $b] = deepclone_from_array($payload, allow_named_closures: true);
    var_dump($a->x); // initializes $a only
})();
// The still-uninitialized ghost of $b pins the whole graph (the context
// holds every object), so even the hydrated $a survives the scope...
echo "scope left\n";
// ...until the cycle collector reclaims the context: $a's destructor runs
// ("dtor 1"), the never-initialized ghost of $b is skipped (no "dtor 2").
gc_collect_cycles();
echo "collected\n";

// ── __wakeup / __unserialize nodes without own closure markers stay eager
//    and may read lazy children ──
class W {
    public ?Node $child = null;
    public string $seen = '';
    public function __wakeup(): void { $this->seen = 'wakeup:' . $this->child->v; }
}
class U {
    public ?Node $child = null;
    public string $seen = '';
    public function __serialize(): array { return ['child' => $this->child]; }
    public function __unserialize(array $data): void {
        $this->child = $data['child'];
        $this->seen = 'unserialize:' . $this->child->v;
    }
}

$w = new W; $w->child = new Node; $w->child->v = 'cw'; $w->child->cb = strlen(...);
$w2 = deepclone_from_array(deepclone_to_array($w, allow_named_closures: true), allow_named_closures: true);
var_dump((new ReflectionClass(W::class))->isUninitializedLazyObject($w2));
var_dump($w2->seen);

$u = new U; $u->child = new Node; $u->child->v = 'cu'; $u->child->cb = strlen(...);
$u2 = deepclone_from_array(deepclone_to_array($u, allow_named_closures: true), allow_named_closures: true);
var_dump((new ReflectionClass(U::class))->isUninitializedLazyObject($u2));
var_dump($u2->seen);

?>
--EXPECT--
NULL
NULL
dtor 1
dtor 2
int(1)
scope left
dtor 1
collected
bool(false)
string(9) "wakeup:cw"
bool(false)
string(14) "unserialize:cu"
