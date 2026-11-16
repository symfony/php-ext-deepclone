--TEST--
ReflectionAttribute works the same with the extension, whatever the instances of attributes hold
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80500) die('skip closures in constant expressions require PHP 8.5+');
?>
--FILE--
<?php

class Validators
{
    public static function check() { return 'checked'; }
}

#[Attribute(Attribute::TARGET_ALL)]
class Tree
{
    public array $nodes = [];

    public function __construct(public $closure = null, int $selfReferences = 0)
    {
        $this->nodes = ['root', new stdClass()];
        for ($i = 0; $i < $selfReferences; ++$i) {
            $this->nodes["self$i"] = &$this->nodes;
        }
    }
}

#[Tree(Validators::check(...), 2)]
class A
{
}

#[Tree(null, 2)]
class B
{
}

// Reading the attributes of a class works the same whatever their instances hold
foreach ([A::class, B::class] as $class) {
    $attribute = (new ReflectionClass($class))->getAttributes()[0];
    $instance = $attribute->newInstance();
    var_dump(count($instance->nodes), count($attribute->getArguments()));
}

// and the callable that A declares is exported as a reference to its declaration
$closure = (new ReflectionClass(A::class))->getAttributes()[0]->newInstance()->closure;
$payload = deepclone_to_array($closure);
echo json_encode($payload['prepared']), ' ', deepclone_from_array($payload)(), "\n";

?>
--EXPECT--
int(4)
int(2)
int(4)
int(2)
["A","",0,0,5] checked
