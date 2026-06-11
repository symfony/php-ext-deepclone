--TEST--
deepclone_from_array() binds shared references to typed properties with type sources
--EXTENSIONS--
deepclone
--FILE--
<?php

class T { public int $a = 0; public int $b = 0; }

// A shared &-ref bound to two typed declared properties used to trip the
// engine's deref assertion on debug builds and skipped type-source
// registration on release builds.
$t = new T;
$x = 5;
$t->a = &$x;
$t->b = &$x;

$t2 = deepclone_from_array(deepclone_to_array($t));

// the reference link survives the round-trip
$r = &$t2->a;
$r = 9;
var_dump($t2->b);

// direct writes keep being type-checked
try {
    $t2->a = 'nope';
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

// and so do writes through the reference (type source registered)
$q = &$t2->b;
try {
    $q = 'nope';
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

// type mismatch inside the payload ref is rejected on hydration
class S { public int $n = 0; public int $m = 0; }
try {
    deepclone_from_array([
        'classes' => 'S',
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['stdClass' => ['n' => [0 => -1], 'm' => [0 => -1]]],
        'resolve' => ['stdClass' => ['n' => [0 => false], 'm' => [0 => false]]],
        'refs' => [1 => 'not-an-int'],
    ]);
    echo "no error?!\n";
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

?>
--EXPECT--
int(9)
Cannot assign string to property T::$a of type int
Cannot assign string to reference held by property T::$a of type int
Cannot assign string to property S::$n of type int
