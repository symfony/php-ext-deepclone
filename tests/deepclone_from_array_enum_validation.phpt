--TEST--
deepclone_from_array() rejects a missing enum case without aborting
--EXTENSIONS--
deepclone
--FILE--
<?php

enum ExistingPayloadEnum { case Present; }

try {
    deepclone_from_array([
        'classes' => '',
        'objectMeta' => 0,
        'prepared' => ['ExistingPayloadEnum::Missing'],
        'mask' => ['e'],
    ]);
    echo "no error?!\n";
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
deepclone_from_array(): malformed payload, enum case "ExistingPayloadEnum::Missing" not found
