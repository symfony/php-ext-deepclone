--TEST--
deepclone_hydrate() with DEEPCLONE_HYDRATE_PRESERVE_REFS on lazy objects
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

class HydrateLazyRefs
{
    public int $a = 0;
    public int $b = 0;
}

$rc = new ReflectionClass(HydrateLazyRefs::class);
$makers = [
    'ghost' => fn () => $rc->newLazyGhost(function (HydrateLazyRefs $o) { $o->a = 5; }),
    'proxy' => fn () => $rc->newLazyProxy(fn () => new HydrateLazyRefs()),
];

// References are bound once the object is initialized; with
// DEEPCLONE_HYDRATE_NO_LAZY_INIT, values are written raw like
// ReflectionProperty::setRawValueWithoutLazyInitialization() does
foreach ($makers as $kind => $make) {
    foreach ([0, DEEPCLONE_HYDRATE_NO_LAZY_INIT] as $flag) {
        $o = $make();
        $x = 1;
        deepclone_hydrate($o, ['a' => &$x, 'b' => &$x], DEEPCLONE_HYDRATE_PRESERVE_REFS | $flag);
        $x = 42;
        echo $kind, $flag ? ' no-lazy-init: ' : ': ', json_encode([$o->a, $o->b, $rc->isUninitializedLazyObject($o)]), "\n";
    }
}
?>
--EXPECT--
ghost: [42,42,false]
ghost no-lazy-init: [1,1,false]
proxy: [42,42,false]
proxy no-lazy-init: [1,1,false]
