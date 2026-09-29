--TEST--
deepclone_to_array() takes the value of references that nothing else holds, like serialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

$y = 1;
$z = 2;
$x = [&$y, &$z, &$z];
unset($y);
echo json_encode(deepclone_to_array($x)), "\n";
?>
--EXPECT--
{"classes":"","objectMeta":0,"prepared":[1,-1,-1],"mask":{"1":false,"2":false},"refs":{"1":2}}
