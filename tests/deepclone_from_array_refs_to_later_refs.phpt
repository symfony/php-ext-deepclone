--TEST--
deepclone_from_array() resolves references whose value holds a reference resolved after them
--EXTENSIONS--
deepclone
--FILE--
<?php

function check($v): void
{
    $c = deepclone_from_array(deepclone_to_array($v, null, true), null, true);
    $c[0]['x'] = 'changed';
    var_dump($c[1]['x'], $c[2]);
}

$x = null;
$arr = ['x' => &$x];
$v = [&$arr, &$arr, &$x];
$x = new stdClass();
unset($arr);
check($v);

$x = null;
$arr = ['x' => &$x];
$v = [&$arr, &$arr, &$x];
$x = strlen(...);
unset($arr);
check($v);

$x = null;
$arr = ['x' => &$x];
$v = [&$arr, &$arr, &$x];
$x = [new stdClass()];
unset($arr);
check($v);

// An array holding a reference to itself
$y = [1];
$y['me'] = &$y;
$x = [$y];
$x[1] = &$x[0];
$c = deepclone_from_array(deepclone_to_array($x));
var_dump($c[1]['me']['me'][0]);
?>
--EXPECT--
string(7) "changed"
string(7) "changed"
string(7) "changed"
string(7) "changed"
string(7) "changed"
string(7) "changed"
int(1)
