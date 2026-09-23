--TEST--
deepclone_to_array() initializes native lazy proxies and exports their real instance's state
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) {
    die('skip native lazy objects require PHP 8.4+');
}
?>
--FILE--
<?php

class Base
{
    private string $secret = 'default';

    public function __construct(public string $value) {}

    public function setSecret(string $secret): void { $this->secret = $secret; }
    public function getSecret(): string { return $this->secret; }
}

class Child extends Base {}

function isLazy(object $o): bool
{
    return (new ReflectionObject($o))->isUninitializedLazyObject($o);
}

// Uninitialized proxy: the factory runs once; the copy is a plain object of
// the proxy's class holding the real instance's state, like serialize()
$factoryCalls = 0;
$proxy = (new ReflectionClass(Child::class))->newLazyProxy(function () use (&$factoryCalls) {
    ++$factoryCalls;
    $instance = new Base('proxied');
    $instance->setSecret('s3cr3t');

    return $instance;
});

$copy = deepclone_from_array(deepclone_to_array($proxy));
var_dump($factoryCalls, isLazy($proxy));
var_dump($copy::class, $copy->value, $copy->getSecret(), isLazy($copy));

// Already-initialized proxy: the factory does not run again
$copy = deepclone_from_array(deepclone_to_array($proxy));
var_dump($factoryCalls, $copy->value);

// The proxy and its real instance stay two distinct objects, like serialize()
$instance = new Base('shared');
$proxy = (new ReflectionClass(Base::class))->newLazyProxy(fn () => $instance);
[$p, $i] = deepclone_from_array(deepclone_to_array([$proxy, $instance]));
var_dump($p === $i, $p->value, $i->value);

// Initialized proxy whose real instance was reset as a lazy ghost
$instance = new Base('orig');
$proxy = (new ReflectionClass(Base::class))->newLazyProxy(fn () => $instance);
(new ReflectionClass(Base::class))->initializeLazyObject($proxy);
(new ReflectionClass(Base::class))->resetAsLazyGhost($instance, fn (Base $o) => $o->__construct('reset'));
var_dump(deepclone_from_array(deepclone_to_array($proxy))->value);

// SKIP_INITIALIZATION_ON_SERIALIZE is about serialize(): like clone,
// deepclone_to_array() initializes regardless
$ghost = (new ReflectionClass(Base::class))->newLazyGhost(fn (Base $o) => $o->__construct('ghost'), ReflectionClass::SKIP_INITIALIZATION_ON_SERIALIZE);
$proxy = (new ReflectionClass(Base::class))->newLazyProxy(fn () => new Base('proxy'), ReflectionClass::SKIP_INITIALIZATION_ON_SERIALIZE);
[$g, $p] = deepclone_from_array(deepclone_to_array([$ghost, $proxy]));
var_dump($g->value, $p->value);

// A throwing factory propagates and leaves the proxy uninitialized
$proxy = (new ReflectionClass(Base::class))->newLazyProxy(function () {
    throw new RuntimeException('boom');
});
try {
    deepclone_to_array($proxy);
} catch (RuntimeException $e) {
    echo $e->getMessage(), "\n";
}
var_dump(isLazy($proxy));
?>
--EXPECT--
int(1)
bool(false)
string(5) "Child"
string(7) "proxied"
string(6) "s3cr3t"
bool(false)
int(1)
string(7) "proxied"
bool(false)
string(6) "shared"
string(6) "shared"
string(5) "reset"
string(5) "ghost"
string(5) "proxy"
boom
bool(true)
