--TEST--
deepclone_from_array() rejects hard references targeting dynamic properties
--EXTENSIONS--
deepclone
--FILE--
<?php

#[AllowDynamicProperties]
class DynamicHardReferenceTarget {}

try {
    deepclone_from_array([
        'classes' => DynamicHardReferenceTarget::class,
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['stdClass' => ['dynamic' => [0 => -1]]],
        'resolve' => ['stdClass' => ['dynamic' => [0 => false]]],
        'refs' => [1 => 3],
    ]);
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
deepclone_from_array(): hard references cannot target dynamic or virtual properties
