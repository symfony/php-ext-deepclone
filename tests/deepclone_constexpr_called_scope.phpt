--TEST--
A callable over a static method called on a child class isn't taken for the one that its parent declares in a constant expression
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80500) die('skip closures in constant expressions require PHP 8.5+');
?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL)]
class When
{
    public function __construct(public $closure) {}
}

class A
{
    #[When(self::check(...))]
    public $p;

    public static function check() { return static::class; }
}

class B extends A
{
}

// The callable of the attribute is called on A
$declared = (new ReflectionProperty(A::class, 'p'))->getAttributes()[0]->getArguments()[0];
$payload = deepclone_to_array($declared);
echo json_encode($payload['prepared']), ' ', deepclone_from_array($payload)(), "\n";

// The same method called on B isn't that callable: static:: resolves to B
$closure = B::check(...);
try {
    deepclone_to_array($closure);
} catch (ValueError $e) {
    echo substr($e->getMessage(), 0, 72), "\n";
}
$payload = deepclone_to_array($closure, null, true);
echo json_encode($payload['prepared']), ' ', deepclone_from_array($payload, null, true)(), "\n";

?>
--EXPECT--
["A","$p",0,0,14] A
deepclone_to_array(): serializing a closure over the named callable "che
["B","check"] B
