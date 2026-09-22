--TEST--
deepclone_from_array() safely rejects a non-integer nested hard-reference slot
--EXTENSIONS--
deepclone
--ENV--
USE_ZEND_ALLOC=0
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
--FILE--
<?php

try {
    deepclone_from_array([
        'classes' => '',
        'objectMeta' => 0,
        'prepared' => ['x' => 'not a reference id'],
        'mask' => ['x' => false],
    ]);
    echo "no error?!\n";
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
deepclone_from_array(): malformed payload, hard-ref slot must be of type int, string given
