--TEST--
deepclone_from_array() rejects a reference-table entry that hard-references itself
--EXTENSIONS--
deepclone
--FILE--
<?php

try {
    deepclone_from_array([
        'classes' => '',
        'objectMeta' => 0,
        'prepared' => -1,
        'refs' => [1 => -1],
        'refMasks' => [1 => false],
    ]);
    echo "not blocked\n";
} catch (ValueError $e) {
    echo "blocked\n";
}
?>
--EXPECT--
blocked
