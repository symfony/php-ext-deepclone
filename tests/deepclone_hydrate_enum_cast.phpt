--TEST--
deepclone_hydrate() casts string/int scalars to the corresponding backed-enum case
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80100) die('skip backed enums require PHP 8.1+');
?>
--FILE--
<?php

enum Suit: string { case Hearts = 'H'; case Spades = 'S'; }
enum Size: int    { case Small = 1;   case Large = 2;   }
enum Plain { case Alpha; } /* non-backed */

class WithEnum
{
    public Suit $s = Suit::Hearts;
    public ?Suit $ns = Suit::Hearts;
    public Size $n = Size::Small;
    public Plain $p = Plain::Alpha;
}

// String → string-backed enum
$o = new WithEnum();
$o = deepclone_hydrate($o, [WithEnum::class => ['s' => 'S']]);
var_dump($o->s === Suit::Spades);

// Int → int-backed enum
$o = new WithEnum();
$o = deepclone_hydrate($o, [WithEnum::class => ['n' => 2]]);
var_dump($o->n === Size::Large);

// Nullable enum + null → stored as null (no cast, unset rule doesn't fire either)
$o = new WithEnum();
$o = deepclone_hydrate($o, [WithEnum::class => ['ns' => null]]);
var_dump($o->ns === null);

// Nullable enum + matching scalar → cast
$o = new WithEnum();
$o = deepclone_hydrate($o, [WithEnum::class => ['ns' => 'S']]);
var_dump($o->ns === Suit::Spades);

// Cross-type scalar (int → string-backed enum): Enum::from() coerces the
// int to its string form, then the lookup fails for an unknown value →
// ValueError. Same path the polyfill takes via Enum::from($value).
$o = new WithEnum();
try {
    deepclone_hydrate($o, [WithEnum::class => ['s' => 5]]);
    var_dump(false);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'not a valid backing value for enum'));
}

// Unknown backing value: matches Enum::from() — ValueError ("X is not a
// valid backing value for enum Suit").
$o = new WithEnum();
try {
    deepclone_hydrate($o, [WithEnum::class => ['s' => 'X']]);
    var_dump(false);
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'not a valid backing value for enum'));
}

// Non-backed enum: no cast applied, engine TypeError on scalar
$o = new WithEnum();
try {
    deepclone_hydrate($o, [WithEnum::class => ['p' => 'Alpha']]);
    var_dump(false);
} catch (\TypeError $e) {
    var_dump(str_contains($e->getMessage(), 'Plain'));
}

// Passing the enum case directly still works (no cast needed)
$o = new WithEnum();
$o = deepclone_hydrate($o, [WithEnum::class => ['s' => Suit::Spades]]);
var_dump($o->s === Suit::Spades);

// CALL_HOOKS gate is per-prop: non-hooked enum-typed props still get cast.
$o = new WithEnum();
$o = deepclone_hydrate($o, [WithEnum::class => ['s' => 'S']], [], DEEPCLONE_HYDRATE_CALL_HOOKS);
var_dump($o->s === Suit::Spades);

// Hooked enum prop under CALL_HOOKS: the cast decision is property-type
// only — hook signature shape doesn't change it. Both strict and wider
// hooks see the enum case (the wider hook's string branch never fires).
if (PHP_VERSION_ID >= 80400) {
    eval('class WithHookedEnumWider {
        public static ?string $lastRaw = null;
        public Suit $s = Suit::Hearts {
            set(Suit|string $v) {
                if (is_string($v)) {
                    self::$lastRaw = "raw:".$v;
                    $this->s = Suit::from($v);
                } else {
                    $this->s = $v;
                }
            }
        }
    }');
    $o = new WithHookedEnumWider();
    $o = deepclone_hydrate($o, [WithHookedEnumWider::class => ['s' => 'S']], [], DEEPCLONE_HYDRATE_CALL_HOOKS);
    var_dump($o->s === Suit::Spades);
    var_dump(WithHookedEnumWider::$lastRaw === null);

    eval('class WithHookedEnumStrict {
        public Suit $s = Suit::Hearts {
            set(Suit $v) { $this->s = $v; }
        }
    }');
    $o = new WithHookedEnumStrict();
    $o = deepclone_hydrate($o, [WithHookedEnumStrict::class => ['s' => 'S']], [], DEEPCLONE_HYDRATE_CALL_HOOKS);
    var_dump($o->s === Suit::Spades);
} else {
    var_dump(true);
    var_dump(true);
    var_dump(true);
}

echo "Done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
