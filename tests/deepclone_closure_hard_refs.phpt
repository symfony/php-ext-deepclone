--TEST--
deepclone_to_array() keeps the marker of named closures held by & references
--EXTENSIONS--
deepclone
--FILE--
<?php

$c = strlen(...);
$a = [&$c, &$c];
$data = deepclone_to_array($a, null, true);
echo json_encode($data), "\n";

$clone = deepclone_from_array($data, null, true);
var_dump($clone[0]('abc'));
$clone[0] = 1;
var_dump($clone[1]);

// Behind references between properties
$o = new stdClass();
$o->a = &$c;
$o->b = &$c;
$data = deepclone_to_array($o, null, true);
echo json_encode($data['refMasks']), "\n";

$clone = deepclone_from_array($data, null, true);
var_dump(($clone->a)('ab'));
$clone->a = 2;
var_dump($clone->b);
?>
--EXPECT--
{"classes":"","objectMeta":0,"prepared":[-1,-1],"mask":[false,false],"refs":{"1":[null,"strlen"]},"refMasks":{"1":0}}
int(3)
int(1)
{"1":0}
int(2)
int(2)
