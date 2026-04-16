--TEST--
deepclone_hydrate() and deepclone_from_array() handle hooked properties safely
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

class HookedProps
{
    private int $backing = 0;

    public int $x {
        get => $this->backing;
        set(int $value) { $this->backing = $value + 1; }
    }
}

$hydrated = deepclone_hydrate('HookedProps', ['x' => 5]);
var_dump($hydrated->x === 6);

$fromArray = deepclone_from_array([
    'classes' => 'HookedProps',
    'objectMeta' => 1,
    'prepared' => 0,
    'properties' => ['stdClass' => ['x' => [0 => 10]]],
]);
var_dump($fromArray->x === 11);

$roundTrip = new HookedProps();
$roundTrip->x = 20;
$clone = deepclone_from_array(deepclone_to_array($roundTrip));
var_dump($clone->x === 21);

class HookedBackingProps
{
    public int $x = 0 {
        set(int $value) { $this->x = $value * 10; }
    }
}

// Non-virtual hooked property: hydrate writes the backing slot directly,
// bypassing the set hook (matches ReflectionProperty::setRawValue).
$bypass = deepclone_hydrate('HookedBackingProps', ['x' => 7]);
var_dump($bypass->x === 7);

$bypassFromArray = deepclone_from_array([
    'classes' => 'HookedBackingProps',
    'objectMeta' => 1,
    'prepared' => 0,
    'properties' => ['stdClass' => ['x' => [0 => 9]]],
]);
var_dump($bypassFromArray->x === 9);

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
