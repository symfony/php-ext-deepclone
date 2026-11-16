--TEST--
Property tables are written before the destructor of the replaced value runs
--EXTENSIONS--
deepclone
--FILE--
<?php

// The destructor of a replaced value can read or write the property that held it

class Reassign
{
    public $owner;
    public function __destruct() { $this->owner->p = 'from destructor'; }
}

class Grow
{
    public $owner;
    public function __destruct() { for ($i = 0; $i < 100; ++$i) { $this->owner->{"p$i"} = $i; } }
}

#[AllowDynamicProperties]
class Dynamic {}

foreach ([Reassign::class, Grow::class] as $class) {
    $o = new stdClass();
    $o->p = new $class();
    $o->p->owner = $o;
    deepclone_hydrate($o, ['p' => 'hydrated']);
    var_dump($o->p, count((array) $o));
}

$o = new Dynamic();
$o->p = new Grow();
$o->p->owner = $o;
$v = 'hydrated';
deepclone_hydrate($o, ['p' => &$v], DEEPCLONE_HYDRATE_PRESERVE_REFS);
var_dump($o->p, count((array) $o));

// A stdClass created from a serialized string holds values before its properties are written
$o = deepclone_from_array([
    'classes' => 'O:8:"stdClass":1:{s:1:"p";O:4:"Grow":1:{s:5:"owner";r:1;}}',
    'objectMeta' => 1,
    'prepared' => 0,
    'properties' => ['stdClass' => ['p' => ['hydrated']]],
]);
var_dump($o->p, count((array) $o));

?>
--EXPECT--
string(15) "from destructor"
int(1)
string(8) "hydrated"
int(101)
string(8) "hydrated"
int(101)
string(8) "hydrated"
int(101)
