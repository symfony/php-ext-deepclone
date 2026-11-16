--TEST--
The allowed classes can be held by references, like with unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

class Foo
{
    public $a = 1;
}

// A foreach by reference leaves a reference in the array
$allowed = ['\Foo', '\stdClass'];
foreach ($allowed as &$name) {
    $name = ltrim($name, '\\');
}

var_dump(unserialize(serialize(new Foo()), ['allowed_classes' => $allowed]) instanceof Foo);
var_dump(deepclone_from_array(deepclone_to_array(new Foo(), $allowed), $allowed) instanceof Foo);
var_dump(deepclone_from_array(deepclone_to_array(new stdClass(), $allowed), $allowed) instanceof stdClass);

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
