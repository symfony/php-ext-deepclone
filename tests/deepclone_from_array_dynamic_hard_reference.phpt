--TEST--
deepclone_from_array() binds hard references to dynamic properties like unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

#[AllowDynamicProperties]
class DynamicHardReferenceTarget {}

class NoDynamicAttributeTarget {}

readonly class NoDynamicPropertiesTarget {}

function payload(string $class): array
{
    return [
        'classes' => $class,
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['stdClass' => ['a' => [0 => -1], 'b' => [0 => -1]]],
        'resolve' => ['stdClass' => ['a' => [0 => false], 'b' => [0 => false]]],
        'refs' => [1 => 3],
    ];
}

$o = deepclone_from_array(payload(DynamicHardReferenceTarget::class));
$o->a = 42;
var_dump($o->b);

$o = deepclone_from_array(payload(NoDynamicAttributeTarget::class));
$o->a = 42;
var_dump($o->b);

try {
    deepclone_from_array(payload(NoDynamicPropertiesTarget::class));
} catch (Error $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
int(42)
int(42)
Cannot create dynamic property NoDynamicPropertiesTarget::$a
