--TEST--
deepclone_from_array() stops at the stack limit on deeply nested values and masks
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) die('skip the stack limit is checked on PHP 8.4+');
?>
--INI--
zend.max_allowed_stack_size=512K
--FILE--
<?php

function nest($leaf, int $depth)
{
    for ($i = 0; $i < $depth; ++$i) {
        $leaf = [$leaf];
    }

    return $leaf;
}

class Unser
{
    public function __unserialize(array $data): void {}
}

$values = nest(0, 20000);
$masks = nest(true, 20000);

$payloads = [
    'mask' => ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => $values, 'mask' => $masks],
    'properties' => ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ['p' => [$values]]], 'resolve' => ['stdClass' => ['p' => [$masks]]]],
    'refs' => ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'refs' => [1 => $values], 'refMasks' => [1 => $masks]],
    'states' => ['classes' => 'Unser', 'objectMeta' => [[0, -1]], 'prepared' => 0, 'states' => [[0, $values, $masks]]],
];

// With named closures allowed, without them, and with a list of allowed classes: these scan the masks differently
foreach ($payloads as $name => $payload) {
    foreach ([[null, true], [null, false], [['stdClass', 'Unser'], true]] as [$allowed, $named]) {
        try {
            deepclone_from_array($payload, $allowed, $named);
            echo "$name: no error\n";
        } catch (Error $e) {
            echo "$name: ", get_class($e), ': ', substr($e->getMessage(), 0, 23), "\n";
        }
    }
}

?>
--EXPECT--
mask: Error: Maximum call stack size
mask: Error: Maximum call stack size
mask: Error: Maximum call stack size
properties: Error: Maximum call stack size
properties: Error: Maximum call stack size
properties: Error: Maximum call stack size
refs: Error: Maximum call stack size
refs: Error: Maximum call stack size
refs: Error: Maximum call stack size
states: Error: Maximum call stack size
states: Error: Maximum call stack size
states: Error: Maximum call stack size
