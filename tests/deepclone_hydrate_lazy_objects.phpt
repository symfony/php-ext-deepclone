--TEST--
deepclone_hydrate() writes lazy objects like other objects, once initialized, and to the real instance of proxies
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) die('skip lazy objects and property hooks require PHP 8.4+');
?>
--FILE--
<?php

enum Suit: string
{
    case Hearts = 'H';
    case Spades = 'S';
}

class Foo
{
    public $plain = 'default';
    public int $typed = 0;
    public Suit $suit = Suit::Hearts;
    public string $hooked = 'x' { set { $this->hooked = strtoupper($value); } }
    public readonly int $ro;
}

$rc = new ReflectionClass(Foo::class);
$state = fn (Foo $o) => [$o->plain, (new ReflectionProperty($o, 'typed'))->isInitialized($o) ? $o->typed : 'uninitialized', $o->suit->name, $o->hooked, $o->ro ?? null];
$vars = ['plain' => 'hydrated', 'typed' => 5, 'suit' => 'S', 'hooked' => 'abc', 'ro' => 6];

echo "Initialized proxy\n";
foreach ([0, DEEPCLONE_HYDRATE_NO_LAZY_INIT, DEEPCLONE_HYDRATE_PRESERVE_REFS, DEEPCLONE_HYDRATE_CALL_HOOKS] as $flags) {
    $real = new Foo();
    $proxy = $rc->newLazyProxy(fn () => $real);
    $rc->initializeLazyObject($proxy);
    deepclone_hydrate($proxy, $vars, $flags);
    echo json_encode($state($proxy)), ' ', json_encode($state($real)), "\n";
}

echo "Proxy and ghost, not initialized yet\n";
foreach ([0, DEEPCLONE_HYDRATE_PRESERVE_REFS, DEEPCLONE_HYDRATE_CALL_HOOKS] as $flags) {
    $real = new Foo();
    $proxy = $rc->newLazyProxy(fn () => $real);
    deepclone_hydrate($proxy, $vars, $flags);
    $ghost = $rc->newLazyGhost(function (Foo $o) { $o->plain = 'initialized'; });
    deepclone_hydrate($ghost, $vars, $flags);
    echo json_encode($state($real)), ' ', json_encode($state($ghost)), "\n";
}

echo "null leaves the property uninitialized\n";
$ghost = $rc->newLazyGhost(function (Foo $o) {});
deepclone_hydrate($ghost, ['typed' => null]);
$proxy = $rc->newLazyProxy(fn () => new Foo());
deepclone_hydrate($proxy, ['typed' => null]);
echo json_encode($state($ghost)), ' ', json_encode($state($proxy)), "\n";

echo "References are bound to the real instance\n";
$real = new Foo();
$proxy = $rc->newLazyProxy(fn () => $real);
$rc->initializeLazyObject($proxy);
$v = 1;
deepclone_hydrate($proxy, ['plain' => &$v, 'typed' => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS);
$v = 2;
echo json_encode($state($proxy)), ' ', json_encode($state($real)), "\n";

?>
--EXPECT--
Initialized proxy
["hydrated",5,"Spades","abc",6] ["hydrated",5,"Spades","abc",6]
["hydrated",5,"Spades","abc",6] ["hydrated",5,"Spades","abc",6]
["hydrated",5,"Spades","abc",6] ["hydrated",5,"Spades","abc",6]
["hydrated",5,"Spades","ABC",6] ["hydrated",5,"Spades","ABC",6]
Proxy and ghost, not initialized yet
["hydrated",5,"Spades","abc",6] ["hydrated",5,"Spades","abc",6]
["hydrated",5,"Spades","abc",6] ["hydrated",5,"Spades","abc",6]
["hydrated",5,"Spades","ABC",6] ["hydrated",5,"Spades","ABC",6]
null leaves the property uninitialized
["default","uninitialized","Hearts","x",null] ["default","uninitialized","Hearts","x",null]
References are bound to the real instance
[2,2,"Hearts","x",null] [2,2,"Hearts","x",null]
