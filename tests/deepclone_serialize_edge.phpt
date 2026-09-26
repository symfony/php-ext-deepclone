--TEST--
deepclone_to_array() edge cases: __serialize without __unserialize, __unserialize without __serialize, __sleep with private
--EXTENSIONS--
deepclone
--FILE--
<?php

// ── __serialize without __unserialize: scoped property fallthrough ──
class ParentNoUnser {
    private string $foo = 'foo';
}
class ChildNoUnser extends ParentNoUnser {
    public string $baz = '';
    private string $bar = '';
    public function __serialize(): array { return ['foo' => 'foo', 'baz' => 'ccc', 'bar' => 'ddd']; }
}

$o = new ChildNoUnser();
$d = deepclone_to_array($o);
// foo is private on ParentNoUnser → scoped to ParentNoUnser
var_dump(isset($d['properties']['ParentNoUnser']['foo']));
// baz is public → scoped to stdClass
var_dump(isset($d['properties']['stdClass']['baz']));
// bar is private on ChildNoUnser → scoped to ChildNoUnser
var_dump(isset($d['properties']['ChildNoUnser']['bar']));
// No __unserialize → properties structure (not states)
var_dump(!isset($d['states']));

// ── __unserialize without __serialize: raw (array) as state ──
class UnserOnly {
    public string $foo = '';
    public function __unserialize(array $data): void { $this->foo = $data['foo'] ?? ''; }
}

$o = new UnserOnly();
$o->foo = 'hello';
$d = deepclone_to_array($o);
// Should have states (negative wakeup = __unserialize)
var_dump(isset($d['states']));
// State props should be raw array, NOT scoped
$state = reset($d['states']);
var_dump(isset($state[1]['foo']));
var_dump($state[1]['foo'] === 'hello');
// Properties should be empty (all in state)
var_dump(empty($d['properties']));

// Round-trip: use fresh data to avoid input mutation issue
$o2 = new UnserOnly();
$o2->foo = 'hello';
$d2 = deepclone_to_array($o2);
$clone = deepclone_from_array($d2);
var_dump($clone->foo === 'hello');

// ── __sleep with private mangled keys ──
class SleepPrivate {
    public string $good = '';
    protected string $foo = '';
    private string $bar = '';
    public function __sleep(): array {
        return ['good', 'foo', "\0*\0foo", "\0" . self::class . "\0bar"];
    }
    public function setAll(string $g, string $f, string $b): void {
        $this->good = $g; $this->foo = $f; $this->bar = $b;
    }
}

$o = new SleepPrivate();
$o->setAll('night', 'afternoon', 'morning');
// Like serialize(), warns that "\0*\0foo" selects foo again
$d = deepclone_to_array($o);
var_dump(isset($d['properties']['stdClass']['good']));
var_dump(isset($d['properties']['SleepPrivate']['foo']));
var_dump(isset($d['properties']['SleepPrivate']['bar']));

// ── __sleep inherited-private exclusion ──
class ParentSleep {
    private string $secret = '';
    public function setSecret(string $v): void { $this->secret = $v; }
}
class ChildSleep extends ParentSleep {
    public string $pub = '';
    // __sleep returns unmangled "secret" — should NOT match parent's private
    public function __sleep(): array { return ['pub', 'secret']; }
}

$o = new ChildSleep();
$o->pub = 'visible';
// A non-default value, as default ones are skipped anyway
$o->setSecret('changed');
$d = deepclone_to_array($o);
var_dump(isset($d['properties']['stdClass']['pub']));
// 'secret' is a private property of ParentSleep, unmangled "secret"
// in __sleep should NOT match it (inherited private exclusion)
var_dump(!isset($d['properties']['ParentSleep']['secret']));

echo "Done\n";
?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)

%s: serialize(): "" is returned from __sleep() multiple times in %s on line %d
bool(true)
bool(true)
bool(true)

Warning: serialize(): "secret" returned as member variable from __sleep() but does not exist in %s on line %d
bool(true)
bool(true)
Done
