--TEST--
deepclone_to_array() and deepclone_from_array() edge cases
--EXTENSIONS--
deepclone
--FILE--
<?php

// ── Empty stdClass ──
$o = new stdClass();
$d = deepclone_to_array($o);
$c = deepclone_from_array($d);
var_dump($c instanceof stdClass);
var_dump((array)$c === []);

// ── Resource nested in array ──
$r = fopen('php://memory', 'r');
try {
    deepclone_to_array([$r]);
} catch (\DeepClone\NotInstantiableException $e) {
    var_dump(str_contains($e->getMessage(), 'resource'));
}
fclose($r);

// ── Non-public __serialize ──
@class_exists('PrivateSerialize') || eval('
class PrivateSerialize {
    private function __serialize(): array { return []; }
    public function __unserialize(array $d): void {}
}');
try {
    @deepclone_to_array(new PrivateSerialize());
} catch (Error $e) {
    var_dump(str_contains($e->getMessage(), 'non-public') || str_contains($e->getMessage(), 'private'));
}

// ── __sleep returning non-string ──
class BadSleep {
    public int $x = 1;
    public function __sleep(): array { return [123]; }
}
$d = deepclone_to_array(new BadSleep());
$c = deepclone_from_array($d);
var_dump($c instanceof BadSleep);

// ── __serialize throwing ──
class ThrowingSerialize {
    public function __serialize(): array { throw new RuntimeException('boom'); }
    public function __unserialize(array $d): void {}
}
try {
    deepclone_to_array(new ThrowingSerialize());
} catch (RuntimeException $e) {
    var_dump($e->getMessage() === 'boom');
}

// ── Deeply nested scalar array (stack safety) ──
$arr = [1];
for ($i = 0; $i < 200; $i++) {
    $arr = [$arr];
}
$d = deepclone_to_array($arr);
var_dump(array_key_exists('value', $d));

// ── Multiple classes with allowedClasses ──
$a = new stdClass(); $a->dt = new DateTime();
$d = deepclone_to_array($a, ['stdClass', 'DateTime']);
$c = deepclone_from_array($d, ['stdClass', 'DateTime']);
var_dump($c->dt instanceof DateTime);

echo "Done\n";
?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
%Abool(true)

Warning: serialize(): BadSleep::__sleep() should return an array only containing the names of instance-variables to serialize in %s on line %d

Warning: serialize(): "123" returned as member variable from __sleep() but does not exist in %s on line %d
bool(true)
bool(true)
bool(true)
bool(true)
Done
