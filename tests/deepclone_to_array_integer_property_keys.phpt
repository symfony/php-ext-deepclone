--TEST--
deepclone_to_array() exports the integer keys that ArrayObject writes to the property table of the object it wraps
--EXTENSIONS--
deepclone
--FILE--
<?php

// Wrapping an object is deprecated as of PHP 8.6
error_reporting(E_ALL & ~E_DEPRECATED);

$o = new stdClass();
$ao = new ArrayObject($o);
$ao[] = 'a';
$ao[5] = 'b';
$o->name = 'c';

$payload = deepclone_to_array($o);
var_dump($payload['properties']);
var_dump(deepclone_from_array($payload));

// The same through the ArrayObject
$clone = deepclone_from_array(deepclone_to_array($ao));
var_dump(count($clone), $clone[5], $clone['name']);

?>
--EXPECTF--
array(1) {
  ["stdClass"]=>
  array(3) {
    [0]=>
    array(1) {
      [0]=>
      string(1) "a"
    }
    [5]=>
    array(1) {
      [0]=>
      string(1) "b"
    }
    ["name"]=>
    array(1) {
      [0]=>
      string(1) "c"
    }
  }
}
object(stdClass)#%d (3) {
  ["0"]=>
  string(1) "a"
  ["5"]=>
  string(1) "b"
  ["name"]=>
  string(1) "c"
}
int(3)
string(1) "b"
string(1) "c"
