--TEST--
deepclone_to_array() exports the state of untouched ghosts created by deepclone_from_array()
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

class Node
{
    public ?Node $next = null;

    public function __construct(public string $name, public ?Closure $cb = null) {}
}

$a = new Node('a', strlen(...));
$a->next = new Node('b', strtoupper(...));
$a->next->next = $a;

$copy = deepclone_from_array(deepclone_to_array($a, allow_named_closures: true), allow_named_closures: true);
var_dump((new ReflectionObject($copy))->isUninitializedLazyObject($copy));

$again = deepclone_from_array(deepclone_to_array($copy, allow_named_closures: true), allow_named_closures: true);
var_dump($again->name, ($again->cb)('abc'), $again->next->name, ($again->next->cb)('abc'), $again->next->next === $again);
?>
--EXPECT--
bool(true)
string(1) "a"
int(3)
string(1) "b"
string(3) "ABC"
bool(true)
