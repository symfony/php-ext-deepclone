--TEST--
deepclone_to_array() evaluates the constant expressions of a class once per call
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80500) die('skip closures in constant expressions require PHP 8.5+');
?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL | Attribute::IS_REPEATABLE)]
class Meta
{
    public function __construct(public $closure = null, public $option = null) {}
}

class Option
{
    public static int $built = 0;
    public function __construct() { ++self::$built; }
}

class Rules
{
    #[Meta(static function () { return 'a'; }, new Option())]
    public function a() {}

    #[Meta('literal', ['arguments', 'are', 'not', 'evaluated'])]
    public function b() {}

    #[Meta(static function () { return 'c'; }, new Option())]
    #[Meta(option: new Option())]
    public function c() {}

    public static function runtime() { return 'runtime'; }
}

$closures = [];
foreach (['a', 'c'] as $method) {
    $closures[$method] = (new ReflectionMethod(Rules::class, $method))->getAttributes()[0]->getArguments()[0];
}

// The constant expressions of a class are evaluated once per call, however many closures reference them
Option::$built = 0;
$graph = [];
for ($i = 0; $i < 100; ++$i) {
    $graph[] = [$closures['a'], $closures['c'], (object) ['rule' => $closures['c']]];
}
$payload = deepclone_to_array($graph);
var_dump(Option::$built);
echo json_encode($payload['prepared'][99][0]), ' ', json_encode($payload['prepared'][99][1]), "\n";

// including for the callables that aren't declared in one
Option::$built = 0;
try {
    deepclone_to_array([Rules::runtime(...), Rules::runtime(...), $closures['a']]);
} catch (ValueError $e) {
    echo substr($e->getMessage(), 0, 75), "\n";
}
var_dump(Option::$built);

$clone = deepclone_from_array($payload);
var_dump($clone[0][0](), $clone[5][2]->rule->__invoke());

?>
--EXPECT--
int(3)
["Rules","a()",0,0,17] ["Rules","c()",0,0,23]
deepclone_to_array(): serializing a closure over the named callable "runtim
int(3)
string(1) "a"
string(1) "c"
