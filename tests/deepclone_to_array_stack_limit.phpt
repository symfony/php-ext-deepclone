--TEST--
deepclone_to_array() throws once when it reaches the stack limit
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) die('skip the stack limit is checked on PHP 8.4+');
?>
--INI--
zend.max_allowed_stack_size=512K
--FILE--
<?php

$static = [1];
$graph = [new stdClass()];
for ($depth = 0; $depth < 20000; ++$depth) {
    $static = [$static];
    $graph = [$graph];
}

// One error is thrown, where the limit is reached
foreach ([$static, $graph, (object) ['p' => $static]] as $value) {
    try {
        deepclone_to_array($value);
        echo "no error\n";
    } catch (Error $e) {
        echo substr($e->getMessage(), 0, 23), ', previous: ', get_debug_type($e->getPrevious()), "\n";
    }
}

?>
--EXPECT--
Maximum call stack size, previous: null
Maximum call stack size, previous: null
Maximum call stack size, previous: null
