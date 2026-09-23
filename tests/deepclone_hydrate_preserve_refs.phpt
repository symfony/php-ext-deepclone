--TEST--
deepclone_hydrate() with DEEPCLONE_HYDRATE_PRESERVE_REFS binds references to dynamic properties
--EXTENSIONS--
deepclone
--FILE--
<?php

#[AllowDynamicProperties]
class HydrateDynamicRefs
{
    public $declared;
}

$x = 1;
$o = deepclone_hydrate(HydrateDynamicRefs::class, ['declared' => &$x, 'a' => &$x, 'b' => &$x], DEEPCLONE_HYDRATE_PRESERVE_REFS);
$x = 42;
var_dump($o->declared, $o->a, $o->b);

$y = 1;
$o = deepclone_hydrate(new stdClass(), ['a' => &$y], DEEPCLONE_HYDRATE_PRESERVE_REFS);
$y = 42;
var_dump($o->a);
?>
--EXPECT--
int(42)
int(42)
int(42)
int(42)
