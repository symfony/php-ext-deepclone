--TEST--
deepclone recovers cross-class first-class-callable provenance from ReflectionAttribute (PHP 8.5, experimental)
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80500) die('skip requires PHP 8.5');
if (method_exists('Closure', '__serialize')) die('skip native const-expr serialization present; capture is disabled');
?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL)]
class When { public function __construct(public mixed $cb) {} }

class Validators {
    public static function check(): bool { return true; }
    public static function other(): bool { return false; }
    public static function loose(): int { return 0; }  // not declared in any attribute
}

class Order {
    #[When(Validators::check(...))] public string $x = '';
    #[When(self::own(...))] public string $z = '';
    private static function own(): int { return 7; }
}

class Account {
    #[When(Validators::other(...))] public string $y = '';
}

function dc_prov_validator(string $s): int { return strlen($s) * 2; }

class Globals {
    #[When(strlen(...))] public string $i = '';           // internal global function
    #[When(dc_prov_validator(...))] public string $u = ''; // user global function
}

function show(string $label, callable $fn): void {
    try { $fn(); echo "$label: (no throw)\n"; }
    catch (\Throwable $e) { echo "$label: ", get_class($e), ": ", $e->getMessage(), "\n"; }
}

echo "== 1. cross-class FCC via getArguments(): references the declaring class ==\n";
$x = (new ReflectionProperty(Order::class, 'x'))->getAttributes()[0]->getArguments()[0];
$d = deepclone_to_array($x);
var_dump($d['mask'] === 1);               // declaration-site reference, not by-name
var_dump($d['prepared'][0] === 'Order');  // the DECLARING class, not Validators (the target's scope)
var_dump($d['prepared'][1] === '$x@0');
$r = deepclone_from_array($d);
var_dump($r instanceof Closure, $r() === true);

echo "== 2. own-class FCC is unaffected ==\n";
$z = (new ReflectionProperty(Order::class, 'z'))->getAttributes()[0]->getArguments()[0];
$dz = deepclone_to_array($z);
var_dump($dz['mask'] === 1, deepclone_from_array($dz)() === 7);

echo "== 3. cross-class FCC via newInstance(): the closure is captured from the attribute instance ==\n";
$when = (new ReflectionProperty(Account::class, 'y'))->getAttributes()[0]->newInstance();
$dy = deepclone_to_array($when->cb);
var_dump($dy['mask'] === 1);
var_dump($dy['prepared'][0] === 'Account');
var_dump(deepclone_from_array($dy)() === false);

echo "== 4. global-function FCCs (internal and user) reference the declaring class ==\n";
$gi = (new ReflectionProperty(Globals::class, 'i'))->getAttributes()[0]->getArguments()[0];
$di = deepclone_to_array($gi);
var_dump($di['mask'] === 1, $di['prepared'][0] === 'Globals');
var_dump(deepclone_from_array($di)('hello') === 5);
$gu = (new ReflectionProperty(Globals::class, 'u'))->getAttributes()[0]->getArguments()[0];
$du = deepclone_to_array($gu);
var_dump($du['mask'] === 1, $du['prepared'][0] === 'Globals');
var_dump(deepclone_from_array($du)('hello') === 10);

echo "== 5. a callable no attribute declares stays by-name (needs the opt-in) ==\n";
show('uncaptured', fn () => deepclone_to_array(Validators::loose(...)));

echo "Done\n";
?>
--EXPECT--
== 1. cross-class FCC via getArguments(): references the declaring class ==
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
== 2. own-class FCC is unaffected ==
bool(true)
bool(true)
== 3. cross-class FCC via newInstance(): the closure is captured from the attribute instance ==
bool(true)
bool(true)
bool(true)
== 4. global-function FCCs (internal and user) reference the declaring class ==
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
== 5. a callable no attribute declares stays by-name (needs the opt-in) ==
uncaptured: ValueError: deepclone_to_array(): serializing a closure over the named callable "loose" requires enabling the "allow_named_closures" option; do it only if you trust the input
Done
