--TEST--
deepclone_hydrate() keeps readonly properties that are initialized, whatever the value or the flags
--EXTENSIONS--
deepclone
--FILE--
<?php

enum Suit: string
{
    case Hearts = 'H';
    case Spades = 'S';
}

class Point
{
    public function __construct(public readonly int $x) {}
}

function check(callable $fn): void
{
    try {
        var_dump($fn());
    } catch (Error $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}

// null doesn't uninitialize a readonly property that was initialized
check(fn () => deepclone_hydrate(new Point(1), ['x' => null])->x);

// Readonly properties are written by value, also with PRESERVE_REFS
check(function () {
    $v = 5;
    $p = deepclone_hydrate(Point::class, ['x' => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS);
    $v = 6;

    return $p->x;
});
check(function () {
    $v = 1;
    $p = deepclone_hydrate(new Point(1), ['x' => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS);
    $v = 6;

    return $p->x;
});
check(function () {
    $v = 2;

    return deepclone_hydrate(new Point(1), ['x' => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS)->x;
});

// Enum cases can't be changed that way
check(fn () => deepclone_hydrate(Suit::Hearts, ['name' => null, 'value' => null]));
check(fn () => serialize(Suit::Hearts));

?>
--EXPECTF--
Error: Cannot modify readonly property Point::$x
int(5)
int(1)
Error: Cannot modify readonly property Point::$x
Error: Cannot modify readonly property Suit::$name
string(19) "E:11:"Suit:Hearts";"
