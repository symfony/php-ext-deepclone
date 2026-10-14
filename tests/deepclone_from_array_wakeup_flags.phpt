--TEST--
deepclone_from_array() reads the wakeup flags of "objectMeta" by their sign, whatever their size
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_INT_SIZE < 8) {
    die('skip needs 64-bit integers');
}
?>
--FILE--
<?php

class Foo { public function __unserialize(array $data): void {} }
class Bar { public function __wakeup(): void { echo "wakeup\n"; } }

foreach ([PHP_INT_MIN, -4294967296, -1, 4294967295, PHP_INT_MAX] as $flag) {
    try {
        echo get_debug_type(deepclone_from_array(['classes' => 'Foo', 'objectMeta' => [[0, $flag]], 'prepared' => 0, 'states' => [[0, []]]])), "\n";
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}

foreach ([4294967296, PHP_INT_MAX, -4294967295] as $flag) {
    try {
        echo get_debug_type(deepclone_from_array(['classes' => 'Bar', 'objectMeta' => [[0, $flag]], 'prepared' => 0, 'states' => [0]])), "\n";
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}

?>
--EXPECT--
Foo
Foo
Foo
deepclone_from_array(): Argument #1 ($data) object 0 of class Foo has an __unserialize() method but "objectMeta" does not flag it for an __unserialize state
deepclone_from_array(): Argument #1 ($data) object 0 of class Foo has an __unserialize() method but "objectMeta" does not flag it for an __unserialize state
wakeup
Bar
wakeup
Bar
deepclone_from_array(): Argument #1 ($data) "states" has a __wakeup entry for object id 0 but "objectMeta" does not flag it for __wakeup
