--TEST--
deepclone_from_array() doesn't convert a BcMath\Number to string before its state is restored
--EXTENSIONS--
deepclone
bcmath
--SKIPIF--
<?php
if (!class_exists('BcMath\\Number')) die('skip BcMath\\Number requires PHP 8.4+');
?>
--FILE--
<?php

// A BcMath\Number is only usable once its state is restored, after the properties of all objects are written

class Box
{
    public string $x = '';
}

try {
    deepclone_from_array([
        'classes' => ['Box', 'BcMath\Number'],
        'objectMeta' => [0, [1, -1]],
        'prepared' => 0,
        'properties' => ['stdClass' => ['x' => [0 => 1]]],
        'resolve' => ['stdClass' => ['x' => [0 => true]]],
        'states' => [[1, ['value' => '1.50']]],
    ]);
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

?>
--EXPECT--
Cannot assign BcMath\Number to property Box::$x of type string
