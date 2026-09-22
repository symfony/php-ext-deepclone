--TEST--
deepclone_from_array() writes an ordinary value when dynamic property input is a PHP reference
--EXTENSIONS--
deepclone
--FILE--
<?php

#[AllowDynamicProperties]
class DynamicReferenceTarget {}

$source = 3;
$result = deepclone_from_array([
    'classes' => DynamicReferenceTarget::class,
    'objectMeta' => 1,
    'prepared' => 0,
    'properties' => ['stdClass' => ['dynamic' => [0 => &$source]]],
]);

var_dump($result->dynamic === 3);
$result->dynamic = 4;
var_dump($source === 3);
?>
--EXPECT--
bool(true)
bool(true)
