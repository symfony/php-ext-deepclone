--TEST--
deepclone_from_array() throws ValueError on malformed input
--EXTENSIONS--
deepclone
--FILE--
<?php

function check(string $label, array $payload): void {
    try {
        deepclone_from_array($payload);
        echo "$label: NO EXCEPTION\n";
    } catch (\ValueError $e) {
        echo "$label: ValueError: ", $e->getMessage(), "\n";
    } catch (\Throwable $e) {
        echo "$label: ", $e::class, ": ", $e->getMessage(), "\n";
    }
}

check('missing classes',     ['objectMeta' => 0, 'prepared' => 0]);
check('missing objectMeta',  ['classes' => 'stdClass', 'prepared' => 0]);
check('missing prepared',    ['classes' => 'stdClass', 'objectMeta' => 0]);
check('classes wrong type',  ['classes' => null, 'objectMeta' => 0, 'prepared' => 0]);
check('classes int entry',   ['classes' => [42], 'objectMeta' => 0, 'prepared' => 0]);
check('objectMeta wrong type', ['classes' => 'stdClass', 'objectMeta' => 'foo', 'prepared' => 0]);
check('objectMeta count negative', ['classes' => 'stdClass', 'objectMeta' => -1, 'prepared' => 0]);
check('objectMeta needs class but classes empty', ['classes' => '', 'objectMeta' => 1, 'prepared' => 0]);
check('cidx out of range',   ['classes' => 'stdClass', 'objectMeta' => [[5, 0]], 'prepared' => 0]);
check('meta wrong shape',    ['classes' => 'stdClass', 'objectMeta' => [['x','y']], 'prepared' => 0]);
check('meta wrong scalar',   ['classes' => 'stdClass', 'objectMeta' => ['foo'], 'prepared' => 0]);
check('properties wrong type', ['classes' => 'stdClass', 'objectMeta' => 0, 'prepared' => 0, 'properties' => 'foo']);
check('states wrong type',   ['classes' => 'stdClass', 'objectMeta' => 0, 'prepared' => 0, 'states' => 'foo']);
check('states entry wrong type', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'states' => [1 => 'foo']]);
check('states refs unknown id', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'states' => [1 => 99]]);
check('properties obj_id out of range',
    ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0,
     'properties' => ['stdClass' => ['x' => [99 => 'val']]]]);
check('prepared object id out of range', ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 99]);
check('prepared ref id unknown', ['classes' => '', 'objectMeta' => 0, 'prepared' => -99]);

// objectMeta flags wakeup but states is missing the matching entry.
check('wakeup advertised but no states',
    ['classes' => 'stdClass', 'objectMeta' => [[0, 1]], 'prepared' => 0]);
check('unserialize advertised but no states',
    ['classes' => 'stdClass', 'objectMeta' => [[0, -1]], 'prepared' => 0]);

// states direction contradicts objectMeta sign.
check('states wakeup entry without positive meta',
    ['classes' => 'stdClass', 'objectMeta' => [[0, -1]], 'prepared' => 0, 'states' => [1 => 0]]);
check('states unserialize entry without negative meta',
    ['classes' => 'stdClass', 'objectMeta' => [[0, 1]], 'prepared' => 0, 'states' => [1 => [0, []]]]);

// states entry references an id whose meta flag is 0.
check('states wakeup entry but meta=0',
    ['classes' => 'stdClass', 'objectMeta' => 1, 'prepared' => 0, 'states' => [1 => 0]]);

echo "Done\n";
?>
--EXPECTF--
missing classes: ValueError: deepclone_from_array(): Argument #1 ($data) is missing required "classes" key
missing objectMeta: ValueError: deepclone_from_array(): Argument #1 ($data) is missing required "objectMeta" key
missing prepared: ValueError: deepclone_from_array(): Argument #1 ($data) is missing required "prepared" key
classes wrong type: ValueError: deepclone_from_array(): Argument #1 ($data) "classes" must be of type string|array, %s given
classes int entry: ValueError: deepclone_from_array(): Argument #1 ($data) "classes" entries must be of type string, %s given
objectMeta wrong type: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" must be of type int|array, %s given
objectMeta count negative: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" count must be non-negative, -1 given
objectMeta needs class but classes empty: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" references class index 0 but "classes" is empty
cidx out of range: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 0 has out-of-range class index 5
meta wrong shape: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 0 must be [int, int]
meta wrong scalar: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 0 must be of type int|array, %s given
properties wrong type: ValueError: deepclone_from_array(): Argument #1 ($data) "properties" must be of type array, %s given
states wrong type: ValueError: deepclone_from_array(): Argument #1 ($data) "states" must be of type array, %s given
states entry wrong type: ValueError: deepclone_from_array(): Argument #1 ($data) "states" entry must be of type int|array, %s given
states refs unknown id: ValueError: deepclone_from_array(): Argument #1 ($data) "states" entry references unknown object id 99
properties obj_id out of range: ValueError: deepclone_from_array(): Argument #1 ($data) "properties" entry for "stdClass::x" references unknown object id 99
prepared object id out of range: ValueError: deepclone_from_array(): Argument #1 ($data) "prepared" references unknown object id 99
prepared ref id unknown: ValueError: deepclone_from_array(): Argument #1 ($data) "prepared" references unknown ref id 99
wakeup advertised but no states: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 0 flags object for state replay but no matching "states" entry was found
unserialize advertised but no states: ValueError: deepclone_from_array(): Argument #1 ($data) "objectMeta" entry 0 flags object for state replay but no matching "states" entry was found
states wakeup entry without positive meta: ValueError: deepclone_from_array(): Argument #1 ($data) "states" has a __wakeup entry for object id 0 but "objectMeta" does not flag it for __wakeup
states unserialize entry without negative meta: ValueError: deepclone_from_array(): Argument #1 ($data) "states" has an __unserialize entry for object id 0 but "objectMeta" does not flag it for __unserialize
states wakeup entry but meta=0: ValueError: deepclone_from_array(): Argument #1 ($data) "states" has a __wakeup entry for object id 0 but "objectMeta" does not flag it for __wakeup
Done
