--TEST--
deepclone_from_array() copies the values that a payload holds by reference, for lazy ghosts too
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) die('skip native lazy objects require PHP 8.4+');
?>
--FILE--
<?php

class Node
{
    public mixed $closure = null;
    public mixed $value = null;
    public int $count = 0;
}

// The references that a payload holds aren't bound to the properties, whether the object is created lazy or not
foreach ([false, true] as $lazy) {
    $value = 'original';
    $count = 1;
    $payload = [
        'classes' => 'Node',
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['stdClass' => ['closure' => [$lazy ? [null, 'strlen'] : null], 'value' => [&$value], 'count' => [&$count]]],
        'resolve' => $lazy ? ['stdClass' => ['closure' => [0]]] : [],
    ];
    $node = deepclone_from_array($payload, null, true);
    var_dump((new ReflectionClass(Node::class))->isUninitializedLazyObject($node));
    $node->value = 'changed';
    $count = 'not an int';
    var_dump($value, $node->count);
    unset($node, $payload);
}

?>
--EXPECT--
bool(false)
string(8) "original"
int(1)
bool(true)
string(8) "original"
int(1)
