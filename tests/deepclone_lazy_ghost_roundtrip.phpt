--TEST--
deepclone_to_array() initializes an existing native lazy ghost
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (!method_exists(ReflectionClass::class, 'newLazyGhost')) {
    die('skip requires native lazy objects');
}
?>
--FILE--
<?php

final class LazyRoundtrip
{
    public function __construct(public string $value) {}
}

$initializations = 0;
$reflection = new ReflectionClass(LazyRoundtrip::class);
$object = $reflection->newLazyGhost(
    static function (LazyRoundtrip $object) use (&$initializations): void {
        $initializations++;
        $object->__construct('test');
    },
);

var_dump((new ReflectionObject($object))->isUninitializedLazyObject($object));
$clone = deepclone_from_array(deepclone_to_array($object));
var_dump($initializations);
var_dump($reflection->isUninitializedLazyObject($object));
var_dump($clone->value);
?>
--EXPECT--
bool(true)
int(1)
bool(false)
string(4) "test"
