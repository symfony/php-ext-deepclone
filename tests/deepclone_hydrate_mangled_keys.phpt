--TEST--
deepclone_hydrate() resolves mangled keys to declared properties, for anonymous classes too, and rejects them for stdClass
--EXTENSIONS--
deepclone
--FILE--
<?php

function check(callable $fn): void
{
    try {
        var_dump($fn());
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}

// The keys of an (array) cast address the properties of an anonymous class too, although its name contains a NUL
$o = new class {
    private $a = 1;
    protected $b = 2;
    public $c = 3;
};
$vars = array_map(fn ($v) => $v * 10, (array) $o);
check(fn () => array_values((array) deepclone_hydrate($o, $vars)));

// A mangled key targets a declared property: a stdClass object has none
check(fn () => deepclone_hydrate(new stdClass(), ["\0*\0a" => 1]));
check(fn () => deepclone_hydrate(new stdClass(), ["\0stdClass\0a" => 1]));
check(fn () => deepclone_hydrate(new stdClass(), ["\0*\0" => 1]));
check(fn () => deepclone_hydrate(new stdClass(), ["\0Foo\0a" => 1]));

class Foo
{
    private $a = 1;
    public static $s = 2;
}

class Bar extends Foo
{
}

check(fn () => deepclone_hydrate(new Bar(), ["\0Foo\0a" => 5]));
check(fn () => deepclone_hydrate(new Bar(), ["\0Bar\0nope" => 5]));
check(fn () => deepclone_hydrate(new Bar(), ["\0*\0s" => 5]));
check(fn () => deepclone_hydrate(new Bar(), ["\0Baz\0a" => 5]));
check(fn () => deepclone_hydrate(new Bar(), ["\0Foo\0a\0b" => 5]));
check(fn () => deepclone_hydrate(new Bar(), ["\0\0a" => 5]));
check(fn () => deepclone_hydrate(new Bar(), ["\0Foo" => 5]));

?>
--EXPECTF--
array(3) {
  [0]=>
  int(10)
  [1]=>
  int(20)
  [2]=>
  int(30)
}
deepclone_hydrate(): Argument #%d ($vars) key scope "stdClass" does not declare a "a" property
deepclone_hydrate(): Argument #%d ($vars) key scope "stdClass" does not declare a "a" property
deepclone_hydrate(): Argument #%d ($vars) key scope "stdClass" does not declare a "" property
deepclone_hydrate(): Argument #%d ($vars) key scope "Foo" is not a parent of "stdClass"
object(Bar)#%d (1) {
  ["a":"Foo":private]=>
  int(5)
}
deepclone_hydrate(): Argument #%d ($vars) key scope "Bar" does not declare a "nope" property
deepclone_hydrate(): Argument #%d ($vars) key scope "Bar" does not declare a "s" property
deepclone_hydrate(): Argument #%d ($vars) key scope "Baz" is not a parent of "Bar"
deepclone_hydrate(): Argument #%d ($vars) contains an invalid mangled key
deepclone_hydrate(): Argument #%d ($vars) contains an invalid mangled key
deepclone_hydrate(): Argument #%d ($vars) contains an invalid mangled key
