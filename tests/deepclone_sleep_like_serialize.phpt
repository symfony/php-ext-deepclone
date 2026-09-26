--TEST--
deepclone_to_array() selects the properties named by __sleep() like serialize(), with the same warnings and in the same order
--EXTENSIONS--
deepclone
--FILE--
<?php

class Sleeper
{
    public $a = 'default';
    protected $b = 'default';
    private $c = 'default';
    public int $typed;
    public $names = [];

    public function set($v): void { $this->a = $this->b = $this->c = $v; }

    public function __sleep(): array { return $this->names; }
}

#[AllowDynamicProperties]
class DynamicSleeper
{
    public $a = 'default';
    public $names = [];

    public function __sleep(): array { return $this->names; }
}

class UnserializingSleeper
{
    public $a = 'default';
    protected $b = 'default';
    private $c = 'default';
    public $names = [];
    public $keys;

    public function set($v): void { $this->a = $this->b = $this->c = $v; }

    public function __sleep(): array { return $this->names; }

    public function __unserialize(array $data): void
    {
        $this->keys = strtr(implode(',', array_keys($data)), ["\0" => '~']);
    }
}

class NotAnArray
{
    public function __sleep() { return 'nope'; }
}

function state($value): string
{
    if (!is_object($value)) {
        return json_encode($value);
    }
    $state = [];
    foreach ((array) $value as $k => $v) {
        if ('names' !== $k) {
            $state[] = strtr($k, ["\0" => '~']).'='.json_encode($v);
        }
    }

    return get_class($value).'{'.implode(', ', $state).'}';
}

function run(callable $f): array
{
    $warnings = [];
    set_error_handler(static function ($type, $message) use (&$warnings) {
        $warnings[] = [$type, $message];

        return true;
    });
    try {
        return [state($f()), $warnings];
    } finally {
        restore_error_handler();
    }
}

function check(string $label, object|array $value): void
{
    $native = run(static fn () => unserialize(serialize($value)));
    $clone = run(static fn () => deepclone_from_array(deepclone_to_array($value)));
    echo $label, ': ', $clone[0], $native === $clone ? '' : "\n  serialize(): ".$native[0], "\n";
    foreach ($clone[1] as [$type, $message]) {
        echo '  ', $message, "\n";
    }
    if ($native[1] !== $clone[1]) {
        echo "  serialize() warnings differ\n";
    }
}

function sleeper(string $class, array $names, string $value = 'changed'): object
{
    $o = new $class();
    $o->set($value);
    $o->names = $names;

    return $o;
}

check('same name twice', sleeper(Sleeper::class, ['a', 'a']));
check('same name twice, default value', sleeper(Sleeper::class, ['a', 'a'], 'default'));
check('bare then mangled protected name', sleeper(Sleeper::class, ['b', "\0*\0b"]));
check('mangled then bare protected name', sleeper(Sleeper::class, ["\0*\0b", 'b']));
check('bare then mangled private name', sleeper(Sleeper::class, ['c', "\0Sleeper\0c"]));
check('missing and repeated names', sleeper(Sleeper::class, ['a', 'x', 'a', 'y']));
check('names in reverse order', sleeper(Sleeper::class, ['c', 'b', 'a']));

$o = sleeper(Sleeper::class, ['a', 'b']);
unset($o->a);
check('unset untyped property', $o);
check('uninitialized typed property', sleeper(Sleeper::class, ['typed', 'typed', 'a']));
check('names that are not strings', sleeper(Sleeper::class, [0, 'a', null]));

$o = new DynamicSleeper();
$o->y = 1;
$o->x = 2;
$o->a = 'changed';
$o->names = ['x', 'y', 'x', 'a'];
check('dynamic properties', $o);

check('__unserialize()', sleeper(UnserializingSleeper::class, ['c', 'b', 'a', "\0*\0b", 'x']));

check('__sleep() not returning an array', [new NotAnArray()]);
?>
--EXPECTF--
same name twice: Sleeper{a="changed", ~*~b="default", ~Sleeper~c="default"}
  serialize(): "a" is returned from __sleep() multiple times
same name twice, default value: Sleeper{a="default", ~*~b="default", ~Sleeper~c="default"}
  serialize(): "a" is returned from __sleep() multiple times
bare then mangled protected name: Sleeper{a="default", ~*~b="changed", ~Sleeper~c="default"}
  serialize(): "" is returned from __sleep() multiple times
mangled then bare protected name: Sleeper{a="default", ~*~b="changed", ~Sleeper~c="default"}
  serialize(): "b" is returned from __sleep() multiple times
bare then mangled private name: Sleeper{a="default", ~*~b="default", ~Sleeper~c="changed"}
  serialize(): "" is returned from __sleep() multiple times
missing and repeated names: Sleeper{a="changed", ~*~b="default", ~Sleeper~c="default"}
  serialize(): "x" returned as member variable from __sleep() but does not exist
  serialize(): "a" is returned from __sleep() multiple times
  serialize(): "y" returned as member variable from __sleep() but does not exist
names in reverse order: Sleeper{a="changed", ~*~b="changed", ~Sleeper~c="changed"}
unset untyped property: Sleeper{a="default", ~*~b="changed", ~Sleeper~c="default"}
  serialize(): "a" returned as member variable from __sleep() but does not exist
uninitialized typed property: Sleeper{a="changed", ~*~b="default", ~Sleeper~c="default"}
names that are not strings: Sleeper{a="changed", ~*~b="default", ~Sleeper~c="default"}
  serialize(): Sleeper::__sleep() should return an array only containing the names of instance-variables to serialize
  serialize(): "0" returned as member variable from __sleep() but does not exist
  serialize(): Sleeper::__sleep() should return an array only containing the names of instance-variables to serialize
  serialize(): "" returned as member variable from __sleep() but does not exist
dynamic properties: DynamicSleeper{a="changed", x=2, y=1}
  serialize(): "x" is returned from __sleep() multiple times
__unserialize(): UnserializingSleeper{a="default", ~*~b="default", ~UnserializingSleeper~c="default", keys="~UnserializingSleeper~c,~*~b,a"}
  serialize(): "" is returned from __sleep() multiple times
  serialize(): "x" returned as member variable from __sleep() but does not exist
__sleep() not returning an array: [null]
  serialize(): NotAnArray::__sleep() should return an array only containing the names of instance-variables to serialize
