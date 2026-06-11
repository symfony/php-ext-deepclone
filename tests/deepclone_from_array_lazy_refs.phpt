--TEST--
deepclone_from_array() lazy hydration keeps shared references correct under any touch order
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
class Typed { public int $a = 0; public ?Typed $peer = null; public ?Closure $cb = null; }

// ── Shared &-ref across two ghosts: mutate through whichever side hydrates
//    first, the other must observe it ──
function buildNodes(): array
{
    $a = new Node; $b = new Node;
    $a->cb = strlen(...); $b->cb = strrev(...);   // makes both nodes ghosts
    $shared = 'initial';
    $a->v = &$shared; $b->v = &$shared;
    $a->peer = $b; $b->peer = $a;
    return deepclone_to_array($a, allow_named_closures: true);
}

foreach (['root-first', 'peer-first'] as $order) {
    $root = deepclone_from_array(buildNodes(), allow_named_closures: true);
    var_dump((new ReflectionClass(Node::class))->isUninitializedLazyObject($root));
    if ($order === 'peer-first') {
        $r = &$root->peer->v;   // hydrates root (peer read), then peer (v read)
    } else {
        $r = &$root->v;         // hydrates root only
    }
    $r = "changed-$order";
    var_dump($root->v, $root->peer->v);
}

// ── Same for typed properties (reference binding with type source) ──
$a = new Typed; $b = new Typed;
$a->cb = strlen(...); $b->cb = strrev(...);
$shared = 1;
$a->a = &$shared; $b->a = &$shared;
$a->peer = $b; $b->peer = $a;
$root = deepclone_from_array(deepclone_to_array($a, allow_named_closures: true), allow_named_closures: true);
var_dump((new ReflectionClass(Typed::class))->isUninitializedLazyObject($root));
$w = &$root->peer->a;
$w = 11;
var_dump($root->a);
try {
    $w = 'nope';
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

// ── Object-ref marker on a ref slot (true marker, negative id) is a
//    by-value snapshot, independent of which consumer resolves first.
//    id 0 aliases the ref (false marker), id 1 snapshots it (true marker);
//    flipping the column insertion order flips the resolution order. ──
foreach ([[0, 1], [1, 0]] as $ids) {
    $col = $res = [];
    foreach ($ids as $id) {
        $col[$id] = -1;
        $res[$id] = ($id === 1);
    }
    $payload = [
        'classes' => 'Node',
        'objectMeta' => 2,
        'prepared' => [0, 1],
        'mask' => [true, true],
        'properties' => ['stdClass' => ['v' => $col]],
        'resolve' => ['stdClass' => ['v' => $res]],
        'refs' => [1 => 10],
    ];
    [$o0, $o1] = deepclone_from_array($payload);
    $r = &$o0->v;  // o0 aliases the ref
    $r = 999;
    var_dump($o1->v); // o1 took a by-value snapshot: must stay 10
}

?>
--EXPECT--
bool(true)
string(18) "changed-root-first"
string(18) "changed-root-first"
bool(true)
string(18) "changed-peer-first"
string(18) "changed-peer-first"
bool(true)
int(11)
Cannot assign string to reference held by property Typed::$a of type int
int(10)
int(10)
