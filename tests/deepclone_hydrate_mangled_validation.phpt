--TEST--
deepclone_hydrate() rejects malformed mangled keys
--EXTENSIONS--
deepclone
--FILE--
<?php

class BadKeys {}

function check_bad_key(string $key): bool
{
    try {
        deepclone_hydrate('BadKeys', [], [$key => 123]);
        return false;
    } catch (ValueError $e) {
        return str_contains($e->getMessage(), 'invalid mangled key');
    }
}

var_dump(check_bad_key("\0broken"));
var_dump(check_bad_key("\0\0x"));
var_dump(check_bad_key("\0*oops"));

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
Done
