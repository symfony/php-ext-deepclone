--TEST--
deepclone_hydrate() verifies declared property types (matching polyfill/setRawValue semantics)
--EXTENSIONS--
deepclone
--FILE--
<?php

class TypedInt
{
    public int $x = 0;
}

class TypedReadonly
{
    public readonly int $v;
}

// A. Strict type mismatch → TypeError
try {
    deepclone_hydrate(TypedInt::class, ['x' => 'hello']);
    echo "A: NO THROW\n";
} catch (\TypeError $e) {
    var_dump(str_contains($e->getMessage(), 'type int'));
}

// B. Coercible scalar → coerced to int (non-strict default)
$o = deepclone_hydrate(TypedInt::class, ['x' => '42']);
var_dump($o->x === 42);

// C. Readonly + wrong type → TypeError
try {
    deepclone_hydrate(TypedReadonly::class, ['v' => 'nope']);
    echo "C: NO THROW\n";
} catch (\TypeError $e) {
    var_dump(str_contains($e->getMessage(), 'type int'));
}

// D. Same via from_array payload
try {
    deepclone_from_array([
        'classes' => TypedInt::class,
        'objectMeta' => 1,
        'prepared' => 0,
        'properties' => [TypedInt::class => ['x' => [0 => 'hello']]],
    ]);
    echo "D: NO THROW\n";
} catch (\TypeError $e) {
    var_dump(str_contains($e->getMessage(), 'type int'));
}

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
Done
