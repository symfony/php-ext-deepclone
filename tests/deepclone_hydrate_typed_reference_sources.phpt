--TEST--
References bound to typed properties are tracked by the property that the class of the object declares
--EXTENSIONS--
deepclone
--FILE--
<?php

// The engine tracks the typed properties that a reference is bound to. These
// cases leave that list in a state that debug builds of PHP assert on.

class Base
{
    public int $a = 0;
    protected int $b = 0;
}

class Child extends Base
{
    public int $a = 1;
    protected int $b = 1;
}

// A reference bound through the scope of a parent class belongs to the property the child redeclares
$v = 5;
$objects = [];
for ($i = 0; $i < 4; ++$i) {
    $objects[] = deepclone_hydrate(new Child(), ["\0Base\0a" => &$v, "\0Base\0b" => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS);
}
$objects = null;
try {
    $v = 'not an int';
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}
var_dump($v);

$clone = deepclone_from_array([
    'classes' => 'Child',
    'objectMeta' => 2,
    'prepared' => [0, 1],
    'mask' => [true, true],
    'properties' => ['Base' => ['b' => [-1, -1]]],
    'resolve' => ['Base' => ['b' => [false, false]]],
    'refs' => [1 => 5],
]);
var_dump($clone);
unset($clone);

// null releases the reference that the property holds
$o = new Base();
$r = &$o->a;
deepclone_hydrate($o, ['a' => null]);
$r = 'free';
var_dump($r, (new ReflectionProperty($o, 'a'))->isInitialized($o));
unset($r);

// The type check can call __toString(), which can bind a reference to the property
class Binder
{
    public static $ref;
    public $target;

    public function __toString(): string
    {
        self::$ref = &$this->target->s;

        return 'coerced';
    }
}

class Text
{
    public string $s = 'initial';
}

$t = new Text();
$b = new Binder();
$b->target = $t;
deepclone_hydrate($t, ['s' => $b]);
var_dump($t->s, Binder::$ref);

echo "done\n";

?>
--EXPECTF--
string(10) "not an int"
array(2) {
  [0]=>
  object(Child)#%d (2) {
    ["a"]=>
    int(1)
    ["b":protected]=>
    &int(5)
  }
  [1]=>
  object(Child)#%d (2) {
    ["a"]=>
    int(1)
    ["b":protected]=>
    &int(5)
  }
}
string(4) "free"
bool(false)
string(7) "coerced"
string(7) "coerced"
done
