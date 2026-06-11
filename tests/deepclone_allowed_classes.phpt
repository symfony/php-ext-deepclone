--TEST--
deepclone_to_array() and deepclone_from_array() $allowedClasses parameter
--EXTENSIONS--
deepclone
--FILE--
<?php

// ── to_array: null allows all ──
$o = new stdClass(); $o->x = 1;
$d = deepclone_to_array($o, null);
var_dump(isset($d['classes']));

// ── to_array: specific class allowed ──
$d = deepclone_to_array($o, ['stdClass']);
var_dump($d['classes'] === 'stdClass');

// ── to_array: case insensitive ──
$d = deepclone_to_array($o, ['STDCLASS']);
var_dump($d['classes'] === 'stdClass');

// ── to_array: class rejected ──
try {
    deepclone_to_array($o, ['DateTime']);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), '"stdClass" is not allowed'));
}

// ── to_array: empty list rejects all ──
try {
    deepclone_to_array($o, []);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'is not allowed'));
}

// ── to_array: invalid entry type ──
try {
    deepclone_to_array($o, [123]);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'class names'));
}

// ── to_array: invalid class name ──
try {
    deepclone_to_array($o, ['not a class']);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'class names'));
}

// ── to_array: Closure allowed ──
$d = deepclone_to_array(strlen(...), ['Closure'], true);
var_dump(isset($d['mask']));

// ── to_array: Closure rejected ──
try {
    deepclone_to_array(strlen(...), [], true);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), '"Closure" is not allowed'));
}

// ── to_array: static values bypass check ──
$d = deepclone_to_array(42, []);
var_dump($d === ['value' => 42]);

// ── from_array: allowed ──
$d = deepclone_to_array($o);
$c = deepclone_from_array($d, ['stdClass']);
var_dump($c->x === 1);

// ── from_array: case insensitive ──
$c = deepclone_from_array($d, ['STDCLASS']);
var_dump($c->x === 1);

// ── from_array: rejected ──
try {
    deepclone_from_array($d, []);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), '"stdClass" is not allowed'));
}

// ── from_array: Closure in mask rejected ──
$d = deepclone_to_array(strlen(...), allow_named_closures: true);
try {
    deepclone_from_array($d, ['stdClass'], true);
} catch (ValueError $e) {
    var_dump(str_contains($e->getMessage(), '"Closure" is not allowed'));
}

// ── from_array: null allows all ──
$d = deepclone_to_array($o);
$c = deepclone_from_array($d, null);
var_dump($c->x === 1);

// ── from_array: static values bypass check ──
$c = deepclone_from_array(['value' => 42], []);
var_dump($c === 42);

// ── from_array: allowed child carries inherited parent-declared private
// state even when the parent isn't listed in $allowedClasses. The scope
// check on "properties" entries (is-a-parent-of obj->ce) is the security
// boundary; allowed_classes is for instantiation. ──
class AllowedParent { private string $secret = ''; public function getSecret(): string { return $this->secret; } }
class AllowedChild extends AllowedParent { public string $pub = ''; }

$c = new AllowedChild();
(function () { $this->secret = 'inherited'; })->bindTo($c, AllowedParent::class)();
$c->pub = 'visible';

$d = deepclone_to_array($c, ['AllowedChild']);
$r = deepclone_from_array($d, ['AllowedChild']);
var_dump($r instanceof AllowedChild);
var_dump($r->getSecret() === 'inherited');
var_dump($r->pub === 'visible');

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
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
