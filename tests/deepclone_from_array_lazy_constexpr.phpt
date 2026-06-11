--TEST--
deepclone_from_array() defers const-expr closure re-evaluation; allow-list gate stays eager
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80500) {
    die('skip closures in constant expressions require PHP 8.5+');
}
?>
--FILE--
<?php

#[SomeAttr(static function (): string { return 'from-attribute'; })]
class Site {}

class Holder { public ?Closure $f = null; }

$holder = new Holder;
$holder->f = (new ReflectionClass(Site::class))->getAttributes()[0]->getArguments()[0];

$payload = deepclone_to_array($holder);

// ── The closure re-evaluation is deferred until first access ──
$lazy = deepclone_from_array($payload);
$rc = new ReflectionClass(Holder::class);
var_dump($rc->isUninitializedLazyObject($lazy));
var_dump(($lazy->f)());
var_dump($rc->isUninitializedLazyObject($lazy));

// ── The const-expr allow-list gate fires eagerly, not at first access:
//    "Site" (the closure's declaring class) is missing from the allow-list ──
try {
    deepclone_from_array($payload, ['Holder', 'Closure']);
    echo "no error?!\n";
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}

// with the declaring class allowed, hydration works lazily
$ok = deepclone_from_array($payload, ['Holder', 'Closure', 'Site']);
var_dump($rc->isUninitializedLazyObject($ok));
var_dump(($ok->f)());

?>
--EXPECT--
bool(true)
string(14) "from-attribute"
bool(false)
deepclone_from_array(): class "Site" is not allowed
bool(true)
string(14) "from-attribute"
