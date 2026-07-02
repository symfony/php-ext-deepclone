--TEST--
deepclone gates closures over named callables behind the allow_named_closures option
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID < 80500) die('skip requires PHP 8.5'); ?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL)]
class When { public function __construct(public mixed $cb) {} }

class Order {
    #[When(self::isStrict(...))]
    public string $a = '';

    #[When(static function (): string { return 'anon'; })]
    public string $b = '';

    private static function isStrict(): bool { return true; }
}

class Helper { public static function pub(): string { return 'pub'; } }

class Holder { public mixed $cb = null; }

function show(string $label, callable $fn): void {
    try { $fn(); }
    catch (\Throwable $e) { echo "$label: ", get_class($e), ": ", $e->getMessage(), "\n"; }
}

$rp = new ReflectionProperty(Order::class, 'a');
$fcc = $rp->getAttributes()[0]->getArguments()[0];          // self::isStrict(...)
$anon = (new ReflectionProperty(Order::class, 'b'))->getAttributes()[0]->getArguments()[0];

echo "== 1. own-method FCC in an attribute uses the const-expr path, no opt-in needed ==\n";
$d = deepclone_to_array($fcc);
var_dump($d['mask'] === 1);                                  // 1 = const-expr reference (safe)
var_dump($d['prepared'][0] === Order::class);
$r = deepclone_from_array($d);
var_dump($r instanceof Closure, $r() === true);

echo "== 2. anonymous closure in an attribute is unaffected ==\n";
$d = deepclone_to_array($anon);
var_dump($d['mask'] === 1);
var_dump(deepclone_from_array($d)() === 'anon');

echo "== 3. a runtime named closure refuses to_array without the opt-in ==\n";
show('strlen', fn () => deepclone_to_array(strlen(...)));
show('Helper::pub', fn () => deepclone_to_array(Helper::pub(...)));

echo "== 4. with the opt-in on both ends it round-trips by name ==\n";
$d = deepclone_to_array(strlen(...), allow_named_closures: true);
var_dump($d['mask'] === 0);                                  // 0 = by-name reference
var_dump($d['prepared'] === [null, 'strlen']);
$r = deepclone_from_array($d, allow_named_closures: true);
var_dump($r('hello') === 5);

echo "== 5. a by-name payload is refused by from_array without the opt-in ==\n";
$d = deepclone_to_array(trim(...), allow_named_closures: true);
show('from_array', fn () => deepclone_from_array($d));

echo "== 6. a hostile system() payload is refused by default ==\n";
$evil = deepclone_to_array(\Closure::fromCallable('system'), allow_named_closures: true);
show('system', fn () => deepclone_from_array($evil));

echo "== 7. a named closure nested in an object graph is refused wholesale (before instantiation) ==\n";
$h = new Holder();
$h->cb = strlen(...);
$d = deepclone_to_array($h, allow_named_closures: true);
show('nested', fn () => deepclone_from_array($d));
$r = deepclone_from_array($d, allow_named_closures: true);
var_dump($r instanceof Holder, ($r->cb)('abcd') === 4);

echo "== 8. allowed_classes still gates Closure even with the opt-in ==\n";
show('Closure excluded', fn () => deepclone_from_array($d, ['Holder'], true));
$r = deepclone_from_array($d, ['Holder', 'Closure'], true);
var_dump(($r->cb)('ab') === 2);

echo "Done\n";
?>
--EXPECT--
== 1. own-method FCC in an attribute uses the const-expr path, no opt-in needed ==
bool(true)
bool(true)
bool(true)
bool(true)
== 2. anonymous closure in an attribute is unaffected ==
bool(true)
bool(true)
== 3. a runtime named closure refuses to_array without the opt-in ==
strlen: ValueError: deepclone_to_array(): serializing a closure over the named callable "strlen" requires enabling the "allow_named_closures" option; do it only if you trust the input
Helper::pub: ValueError: deepclone_to_array(): serializing a closure over the named callable "pub" requires enabling the "allow_named_closures" option; do it only if you trust the input
== 4. with the opt-in on both ends it round-trips by name ==
bool(true)
bool(true)
bool(true)
== 5. a by-name payload is refused by from_array without the opt-in ==
from_array: ValueError: deepclone_from_array(): resolving a closure over a named callable requires enabling the "allow_named_closures" option
== 6. a hostile system() payload is refused by default ==
system: ValueError: deepclone_from_array(): resolving a closure over a named callable requires enabling the "allow_named_closures" option
== 7. a named closure nested in an object graph is refused wholesale (before instantiation) ==
nested: ValueError: deepclone_from_array(): resolving a closure over a named callable requires enabling the "allow_named_closures" option
bool(true)
bool(true)
== 8. allowed_classes still gates Closure even with the opt-in ==
Closure excluded: ValueError: deepclone_from_array(): class "Closure" is not allowed
bool(true)
Done
