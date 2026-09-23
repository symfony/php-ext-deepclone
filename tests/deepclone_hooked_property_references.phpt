--TEST--
References targeting hooked properties are written as values, and rejected on virtual ones by deepclone_from_array()
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) {
    die('skip property hooks require PHP 8.4+');
}
?>
--FILE--
<?php

class HookedReferenceTarget
{
    public int $backed = 0 {
        set => $value * 10;
    }

    public int $virtual {
        get => 7;
        set {}
    }
}

function payload(string $property): array
{
    return [
        'classes' => HookedReferenceTarget::class,
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => ['stdClass' => [$property => [0 => -1]]],
        'resolve' => ['stdClass' => [$property => [0 => false]]],
        'refs' => [1 => 3],
    ];
}

var_dump(deepclone_from_array(payload('backed'))->backed);

try {
    deepclone_from_array(payload('virtual'));
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}

$x = 1;
$o = deepclone_hydrate(HookedReferenceTarget::class, ['backed' => &$x, 'virtual' => &$x], DEEPCLONE_HYDRATE_PRESERVE_REFS);
$x = 2;
var_dump($o->backed, $o->virtual);
?>
--EXPECT--
int(3)
deepclone_from_array(): hard references cannot target virtual properties or dynamic properties behind custom handlers
int(1)
int(7)
