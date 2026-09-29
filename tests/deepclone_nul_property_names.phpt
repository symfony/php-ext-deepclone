--TEST--
deepclone_from_array() rejects property names that start with a NUL byte
--EXTENSIONS--
deepclone
--FILE--
<?php

class A
{
    public $a;
}

$payloads = [
    ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ["\0x" => [1]]]],
    ['classes' => 'A', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ["\0A\0a" => [1]]]],
    // An array cast can give such names to a stdClass
    deepclone_to_array((object) ["\0*\0p" => 1]),
];

foreach ($payloads as $payload) {
    try {
        deepclone_from_array($payload);
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}
?>
--EXPECT--
deepclone_from_array(): Argument #1 ($data) "properties" names of scope "stdClass" cannot start with "\0"
deepclone_from_array(): Argument #1 ($data) "properties" names of scope "stdClass" cannot start with "\0"
deepclone_from_array(): Argument #1 ($data) "properties" names of scope "stdClass" cannot start with "\0"
