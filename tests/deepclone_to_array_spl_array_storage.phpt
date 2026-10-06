--TEST--
Writing to an ArrayObject or ArrayIterator after deepclone_to_array() leaves the payload unchanged
--EXTENSIONS--
deepclone
--FILE--
<?php

$ao = new ArrayObject([1, 'list' => [1]]);
$ai = new RecursiveArrayIterator([1]);
$empty = new ArrayObject([1]);
unset($empty[0]);
$data = deepclone_to_array([$ao, $ai, $empty]);

$ao[] = 2;
$ao['list'][] = 2;
$ai[] = 2;
$empty[] = 2;

$clone = deepclone_from_array($data);
var_dump($clone[0]->getArrayCopy() === [1, 'list' => [1]]);
var_dump($clone[1]->getArrayCopy() === [1]);
var_dump($clone[2]->getArrayCopy() === []);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
