--TEST--
deepclone_from_array() gives the value of its target to a reference marked as a hard reference, and rejects the ones that target themselves
--EXTENSIONS--
deepclone
--FILE--
<?php

// A hard-ref marker on the value of a reference: the reference takes the value of its target

function check(array $refs, array $refMasks): void
{
    try {
        $r = deepclone_from_array([
            'classes' => '',
            'objectMeta' => 0,
            'prepared' => [-1, -1],
            'mask' => [false, false],
            'refs' => $refs,
            'refMasks' => $refMasks,
        ], []);
        echo json_encode($r), "\n";
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}

check([1 => -2, 2 => 'v'], [1 => false]);
check([1 => -1], [1 => false]);
check([1 => -1, 2 => -1], [2 => false, 1 => false]);
check([1 => -2, 2 => -1], [1 => false, 2 => false]);
check([1 => -2, 2 => -3, 3 => -1], [1 => false, 2 => false, 3 => false]);

// No id can reference a string key, whatever its hash: on 64-bit, this integer key is the hash of "abc"
check(['abc' => 'x', (PHP_INT_SIZE < 8 ? -1 : -9223372036661289845) => 'y', 1 => 'z'], []);
check(['abc' => -1, 1 => 'z'], ['abc' => false]);

?>
--EXPECT--
["v","v"]
deepclone_from_array(): malformed payload, ref id 1 cannot reference itself
deepclone_from_array(): malformed payload, ref id 1 cannot reference itself
[-1,-1]
[-3,-3]
["z","z"]
["z","z"]
