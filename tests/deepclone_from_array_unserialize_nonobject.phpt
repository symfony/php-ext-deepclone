--TEST--
deepclone_from_array() rejects unserialize results that are not objects (type confusion)
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

// A class-name string whose second byte is ':' is replayed through
// unserialize(). The decoder later dereferences the result as a zend_object*,
// so a scalar/array serialize form (i:, b:, d:, s:, a:) must be rejected
// rather than treated as an object pointer.
check('int',    ['classes' => 'i:1234;',        'objectMeta' => 1, 'prepared' => 0]);
// bool uses %s in --EXPECTF--: zend_zval_value_name() reports "bool" on PHP 8.2
// but "true"/"false" on 8.3+.
check('bool',   ['classes' => 'b:1;',           'objectMeta' => 1, 'prepared' => 0]);
check('float',  ['classes' => 'd:1.5;',         'objectMeta' => 1, 'prepared' => 0]);
check('string', ['classes' => 's:3:"abc";',     'objectMeta' => 1, 'prepared' => 0]);
check('array',  ['classes' => 'a:1:{i:0;i:0;}', 'objectMeta' => 1, 'prepared' => 0]);

echo "Done\n";
?>
--EXPECTF--
int: ValueError: deepclone_from_array(): Argument #1 ($data) object 0 did not unserialize to an object, int given
bool: ValueError: deepclone_from_array(): Argument #1 ($data) object 0 did not unserialize to an object, %s given
float: ValueError: deepclone_from_array(): Argument #1 ($data) object 0 did not unserialize to an object, float given
string: ValueError: deepclone_from_array(): Argument #1 ($data) object 0 did not unserialize to an object, string given
array: ValueError: deepclone_from_array(): Argument #1 ($data) object 0 did not unserialize to an object, array given
Done
