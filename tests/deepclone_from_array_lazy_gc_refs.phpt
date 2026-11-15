--TEST--
deepclone_from_array() lazy hydration: the GC collects graphs whose references hold lazy ghosts
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

class Node { public $v; public ?Closure $cb = null; }

$a = new Node;
$a->cb = strlen(...);
$a->v = [&$a];
$payload = deepclone_to_array($a, allow_named_closures: true);
unset($a);

foreach ([false, true] as $init) {
    (function () use ($payload, $init, &$weak) {
        $a = deepclone_from_array($payload, allow_named_closures: true);
        $weak = WeakReference::create($a);
        var_dump((new ReflectionClass(Node::class))->isUninitializedLazyObject($a));
        if ($init) {
            var_dump($a->v[0] === $a);
        }
    })();
    gc_collect_cycles();
    var_dump($weak->get());
}

?>
--EXPECT--
bool(true)
NULL
bool(true)
bool(true)
NULL
