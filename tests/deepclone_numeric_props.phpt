--TEST--
deepclone_to_array() / deepclone_from_array() round-trip numeric property names
--EXTENSIONS--
deepclone
--FILE--
<?php

// Numeric property names (GH-64548): an object property like $o->{'999'} must
// round-trip the same way serialize()/unserialize() handles it. PHP normalizes
// numeric string array keys to integers, so the payload uses an int key — which
// must survive a var_export()/require or JSON round-trip too.

function rt($v) {
    $arr = deepclone_to_array($v);
    $direct = deepclone_from_array($arr);
    $viaExport = deepclone_from_array(eval('return '.var_export($arr, true).';'));
    $viaJson = deepclone_from_array(json_decode(json_encode($arr), true));
    return [$arr, $direct, $viaExport, $viaJson];
}

// ── stdClass with a numeric property ──
$cfg = new stdClass();
$cfg->{'999'} = ['TST'];
[$arr, $a, $b, $c] = rt($cfg);
// Canonical int key in the payload, matching the polyfill and (array) cast.
var_dump(array_key_first($arr['properties']['stdClass']));   // int(999)
var_dump($a == $cfg, $b == $cfg, $c == $cfg);                // true x3
var_dump($a->{'999'});                                        // array(1){ [0]=> "TST" }

// ── Numeric "0" alongside a normal property ──
$z = new stdClass();
$z->{'0'} = 'zero';
$z->normal = 1;
[, $a, $b, $c] = rt($z);
var_dump($a == $z, $b == $z, $c == $z);
var_dump($a->{'0'} === 'zero' && $a->normal === 1);

// ── Leading-zero "007" is not a canonical int key: stays a string ──
$m = new stdClass();
$m->{'007'} = 'keepstr';
$m->{'8'} = 'int';
[$arr, $a] = rt($m);
$keys = array_keys($arr['properties']['stdClass']);
var_dump($keys);                 // ["007" (string), 8 (int)]
var_dump($a == $m);

// ── Numeric dynamic property on a typed (non-stdClass) object ──
#[AllowDynamicProperties]
class Foo { public int $a = 1; }
$f = new Foo();
$f->{'999'} = ['TST'];
$f->a = 5;
[, $a, $b, $c] = rt($f);
var_dump($a == $f, $b == $f, $c == $f);

// ── Shared-object identity preserved through a numeric property ──
$shared = new stdClass();
$shared->x = 1;
$o = new stdClass();
$o->{'42'} = $shared;
$o->ref2 = $shared;
$rt = deepclone_from_array(deepclone_to_array($o));
var_dump($rt->{'42'} === $rt->ref2);

// ── Hard reference preserved through a numeric property ──
$r = new stdClass();
$val = 7;
$r->{'5'} = &$val;
$r->alias = &$val;
$rt = deepclone_from_array(deepclone_to_array($r));
$rt->{'5'} = 99;
var_dump($rt->alias === 99);

// ── Parity with serialize(): same observable result ──
var_dump(unserialize(serialize($cfg)) == deepclone_from_array(deepclone_to_array($cfg)));

echo "OK\n";
?>
--EXPECT--
int(999)
bool(true)
bool(true)
bool(true)
array(1) {
  [0]=>
  string(3) "TST"
}
bool(true)
bool(true)
bool(true)
bool(true)
array(2) {
  [0]=>
  string(3) "007"
  [1]=>
  int(8)
}
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK
