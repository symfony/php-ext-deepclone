--TEST--
deepclone_from_array() returns ordinary values from unmarked top-level PHP references
--EXTENSIONS--
deepclone
--FILE--
<?php

function check(array $data, int &$source): void
{
    $result = deepclone_from_array($data);
    var_dump($result === 3);
    $result = 4;
    var_dump($source === 3);
}

$source = 3;
check(['value' => &$source], $source);

$source = 3;
check([
    'classes' => '',
    'objectMeta' => 0,
    'prepared' => &$source,
], $source);

$source = 3;
check([
    'classes' => '',
    'objectMeta' => 0,
    'prepared' => &$source,
    'mask' => null,
], $source);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
