--TEST--
deepclone_to_array() keeps the marker of closures declared in constant expressions held by & references (PHP 8.5)
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID < 80500) die('skip requires PHP 8.5'); ?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL)]
class HardRefAttr { public function __construct(public Closure $c) {} }

class HardRefHolder
{
    #[HardRefAttr(static function () { return 'called'; })]
    public $p;
}

$c = (new ReflectionProperty(HardRefHolder::class, 'p'))->getAttributes()[0]->newInstance()->c;
$a = [&$c, &$c];
$data = deepclone_to_array($a);
echo json_encode($data['refMasks']), "\n";

$clone = deepclone_from_array($data);
var_dump($clone[0]());
$clone[0] = 1;
var_dump($clone[1]);
?>
--EXPECT--
{"1":1}
string(6) "called"
int(1)
