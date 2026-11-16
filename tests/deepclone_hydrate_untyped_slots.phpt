--TEST--
deepclone_hydrate() writes untyped properties like the engine when they were unset or hold a reference
--EXTENSIONS--
deepclone
--FILE--
<?php

class Magic
{
    public $a = 1;
    public int $t = 1;
    public array $log = [];

    public function __construct() { unset($this->a, $this->t); }
    public function __set($name, $value) { $this->log[] = $name; $this->$name = $value; }
}

// Like ReflectionProperty::setRawValue(), __set() is called for the properties that were unset
$o = deepclone_hydrate(new Magic(), ['a' => 2, 't' => 3]);
var_dump($o->a, $o->t, $o->log);

class Plain
{
    public $u = 1;
    public int $i = 1;
}

// A value is written through the reference that a property holds
$o = new Plain();
$u = &$o->u;
$i = &$o->i;
deepclone_hydrate($o, ['u' => 5, 'i' => 5]);
var_dump($u, $i);

// With PRESERVE_REFS, the property is bound to the new reference
$v = 7;
deepclone_hydrate($o, ['u' => &$v, 'i' => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS);
$v = 8;
var_dump($o->u, $o->i);

?>
--EXPECTF--
int(2)
int(3)
array(2) {
  [0]=>
  string(1) "a"
  [1]=>
  string(1) "t"
}
int(5)
int(5)
int(8)
int(8)
