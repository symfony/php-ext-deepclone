--TEST--
deepclone_from_array() replays the states of internal objects that are unusable until then before any user code runs
--EXTENSIONS--
deepclone
bcmath
--SKIPIF--
<?php
if (!class_exists('BcMath\\Number')) die('skip BcMath\\Number requires PHP 8.4+');
?>
--FILE--
<?php

class Holder
{
    public string $s;

    public function __unserialize(array $data): void
    {
        echo "Holder::__unserialize()\n";
        // Not in strict mode: the value is converted to string
        $this->s = $data['s'];
    }
}

class Waker
{
    public $n;

    public function __wakeup(): void
    {
        echo "Waker::__wakeup() ", $this->n, "\n";
    }
}

class Setter
{
    public function __set($name, $value)
    {
        echo "Setter::__set() ", $value, "\n";
    }
}

function check(array $payload): void
{
    try {
        $o = deepclone_from_array($payload);
        echo json_encode((array) $o), "\n";
    } catch (Throwable $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}

// A BcMath\Number can't be read before its state is replayed: it's replayed first, whatever the order of the states
check(['classes' => ['Holder', 'BcMath\\Number'], 'objectMeta' => [[0, -1], [1, -2]], 'prepared' => 0, 'states' => [1 => [0, ['s' => 1], ['s' => true]], 2 => [1, ['value' => '1']]]]);
check(['classes' => ['Waker', 'BcMath\\Number'], 'objectMeta' => [[0, 1], [1, -2]], 'prepared' => 0, 'properties' => ['stdClass' => ['n' => [0 => 1]]], 'resolve' => ['stdClass' => ['n' => [0 => true]]], 'states' => [1 => 0, 2 => [1, ['value' => '2']]]]);
check(['classes' => ['Setter', 'BcMath\\Number'], 'objectMeta' => [0, [1, -1]], 'prepared' => 0, 'properties' => ['stdClass' => ['x' => [0 => 1]]], 'resolve' => ['stdClass' => ['x' => [0 => true]]], 'states' => [1 => [1, ['value' => '3']]]]);

// No user code runs when the state is invalid or missing
check(['classes' => ['Holder', 'BcMath\\Number'], 'objectMeta' => [[0, -1], [1, -2]], 'prepared' => 0, 'states' => [1 => [0, ['s' => 1], ['s' => true]], 2 => [1, ['value' => 0], ['value' => true]]]]);
check(['classes' => ['Holder', 'BcMath\\Number'], 'objectMeta' => [[0, -1], [1, -2]], 'prepared' => 0, 'states' => [1 => [0, ['s' => 1], ['s' => true]]]]);
check(['classes' => ['Holder', 'BcMath\\Number'], 'objectMeta' => [[0, -1], [1, -2]], 'prepared' => 0, 'states' => [1 => [0, ['s' => 1], ['s' => true]], 2 => [1]]]);

?>
--EXPECT--
Holder::__unserialize()
{"s":"1"}
Waker::__wakeup() 2
{"n":{"value":"2","scale":0}}
Setter::__set() 3
[]
Exception: Invalid serialization data for BcMath\Number object
ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 1 flags object for state replay but no matching "states" entry was found
ValueError: deepclone_from_array(): Argument #1 ($data) malformed "states" entry: expected [int, mixed, mixed?]
