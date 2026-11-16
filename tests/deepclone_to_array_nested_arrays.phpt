--TEST--
deepclone_to_array() walks nested arrays that hold objects
--EXTENSIONS--
deepclone
--FILE--
<?php

// Nested arrays that hold an object at the bottom: each level is copied, with its scalars
$graph = [new stdClass()];
for ($depth = 0; $depth < 200; ++$depth) {
    $graph = [$depth, 'scalar', [1, 2, 3], $graph];
}

$payload = deepclone_to_array($graph);
$clone = deepclone_from_array($payload);
var_dump($clone == $graph, $clone !== $graph);

$inner = $clone;
for ($depth = 199; $depth >= 0; --$depth) {
    if ($inner[0] !== $depth) {
        echo "unexpected value at depth $depth\n";
    }
    $inner = $inner[3];
}
var_dump($inner);

// The same array met again after a scan found it holds an object
$shared = [[new stdClass()]];
$clone = deepclone_from_array(deepclone_to_array([$shared, [$shared], $shared]));
var_dump($clone[0][0][0] === $clone[1][0][0][0], $clone[2][0][0] === $clone[0][0][0]);

?>
--EXPECTF--
bool(true)
bool(true)
array(1) {
  [0]=>
  object(stdClass)#%d (0) {
  }
}
bool(true)
bool(true)
