--TEST--
deepclone_from_array() reports malformed ids by their key and rejects masks that match no value
--EXTENSIONS--
deepclone
--FILE--
<?php

function check(string $label, array $payload): void
{
    try {
        $r = deepclone_from_array($payload);
        echo "$label: ", json_encode($r), "\n";
    } catch (ValueError $e) {
        echo "$label: ", $e->getMessage(), "\n";
    }
}

check('objectMeta string key', ['classes' => 'stdClass', 'objectMeta' => ['k' => 0], 'prepared' => 0]);
check('objectMeta negative key', ['classes' => 'stdClass', 'objectMeta' => [-1 => 0], 'prepared' => 0]);
check('properties string id', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ['a' => ['k' => 1]]]]);
check('properties negative id', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ['a' => [-2 => 1]]]]);
check('properties id out of range', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ['a' => [1, 5 => 2]]]]);

// A mask entry that matches no value resolves null, which masks reject
check('array mask', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => [0], 'mask' => [1 => true]]);
check('array mask hard ref', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => [0], 'mask' => [1 => false]]);
check('refMasks without refs', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'refMasks' => [1 => [true]]]);
check('refMasks without ref', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'refs' => [1 => 0], 'refMasks' => [2 => true]]);
check('refMasks', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => [-1], 'mask' => [false], 'refs' => [1 => 0], 'refMasks' => [1 => true]]);
?>
--EXPECT--
objectMeta string key: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry index k out of range
objectMeta negative key: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry index -1 out of range
properties string id: deepclone_from_array(): Argument #1 ($data) "properties" entry for "stdClass::a" references unknown object id k
properties negative id: deepclone_from_array(): Argument #1 ($data) "properties" entry for "stdClass::a" references unknown object id -2
properties id out of range: deepclone_from_array(): Argument #1 ($data) "properties" entry for "stdClass::a" references unknown object id 5
array mask: deepclone_from_array(): malformed payload, object reference value must be of type int, null given
array mask hard ref: deepclone_from_array(): malformed payload, hard-ref slot must be of type int, null given
refMasks without refs: deepclone_from_array(): malformed payload, array-mask value must be of type array, null given
refMasks without ref: deepclone_from_array(): malformed payload, object reference value must be of type int, null given
refMasks: [{}]
