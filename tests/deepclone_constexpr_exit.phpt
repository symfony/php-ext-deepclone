--TEST--
deepclone_to_array() doesn't ignore exit() when it evaluates constant expressions
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80500) die('skip closures in constant expressions require PHP 8.5+');
?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL)]
class Meta
{
    public function __construct(public $value = null) {}
}

class Quit
{
    public function __construct() { echo "exiting\n"; exit(3); }
}

class Fail
{
    public function __construct() { throw new RuntimeException('ignored, like the sites that fail to evaluate'); }
}

#[Meta(new Fail())]
class Failing
{
    #[Meta(static function () { return 'found'; })]
    public function m() {}
}

#[Meta(new Quit())]
class Quitting
{
    #[Meta(static function () {})]
    public function m() {}
}

$closure = (new ReflectionMethod(Failing::class, 'm'))->getAttributes()[0]->getArguments()[0];
echo deepclone_from_array(deepclone_to_array($closure))(), "\n";

// exit() isn't an exception to ignore
$closure = (new ReflectionMethod(Quitting::class, 'm'))->getAttributes()[0]->getArguments()[0];
deepclone_to_array($closure);
echo "not reached\n";

?>
--EXPECT--
found
exiting
