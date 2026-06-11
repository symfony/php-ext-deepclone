--TEST--
deepclone_to_array() handles named closures
--EXTENSIONS--
deepclone
--FILE--
<?php

class ClosureTest {
    public function instanceMethod(): string { return 'instance'; }
    public static function staticMethod(): string { return 'static'; }
    private function privateMethod(): string { return 'private'; }
    public function getPrivateClosure(): \Closure { return $this->privateMethod(...); }
}

// ── Global function ──
$d = deepclone_to_array(strlen(...), allow_named_closures: true);
var_dump($d['mask'] === 0);
var_dump($d['prepared'][0] === null);
var_dump($d['prepared'][1] === 'strlen');
$clone = deepclone_from_array($d, allow_named_closures: true);
var_dump($clone('hello') === 5);

// ── Static method ──
$d = deepclone_to_array(ClosureTest::staticMethod(...), allow_named_closures: true);
var_dump($d['prepared'][0] === 'ClosureTest');
var_dump($d['prepared'][1] === 'staticMethod');
$clone = deepclone_from_array($d, allow_named_closures: true);
var_dump($clone() === 'static');

// ── Instance method ──
$obj = new ClosureTest();
$d = deepclone_to_array($obj->instanceMethod(...), allow_named_closures: true);
var_dump($d['classes'] === 'ClosureTest');
$clone = deepclone_from_array($d, allow_named_closures: true);
var_dump($clone() === 'instance');

// ── Private method ──
$obj = new ClosureTest();
$fn = $obj->getPrivateClosure();
$d = deepclone_to_array($fn, allow_named_closures: true);
var_dump($d['mask'] === 0); // named closure marker
$clone = deepclone_from_array($d, allow_named_closures: true);
var_dump($clone() === 'private');

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
Done
