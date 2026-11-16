--TEST--
deepclone_hydrate() separates the property table that a stdClass object shares with a clone or an array
--EXTENSIONS--
deepclone
--FILE--
<?php

// A clone shares the property table of its origin until one of them is written
$a = new stdClass();
$a->p = 1;
$b = clone $a;
deepclone_hydrate($b, ['p' => 2, 'q' => 3]);
var_dump($a, $b);

// So does the array an object is cast from, or to
$row = ['id' => random_int(1, 1)];
$o = (object) $row;
deepclone_hydrate($o, ['name' => 'x']);
var_dump($row);

$o = new stdClass();
$o->a = 1;
$copy = (array) $o;
$v = 2;
deepclone_hydrate($o, ['b' => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS);
var_dump($copy);

function literal() { return (object) ['a' => 1]; }
deepclone_hydrate(literal(), ['b' => 2]);
var_dump(literal());

?>
--EXPECTF--
object(stdClass)#%d (1) {
  ["p"]=>
  int(1)
}
object(stdClass)#%d (2) {
  ["p"]=>
  int(2)
  ["q"]=>
  int(3)
}
array(1) {
  ["id"]=>
  int(1)
}
array(1) {
  ["a"]=>
  int(1)
}
object(stdClass)#%d (1) {
  ["a"]=>
  int(1)
}
